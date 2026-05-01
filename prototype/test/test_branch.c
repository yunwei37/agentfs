/*
 * test_branch.c — userspace driver for the branch() syscall.
 *
 * Drives the kernel branch(BR_CREATE/BR_COMMIT/BR_ABORT) syscall over a
 * BranchFS mount and verifies the file-level outcome:
 *   - committed branches' files appear in /base
 *   - aborted branches' files do not appear in /base
 *   - first-commit-wins kills sibling branches
 *
 * Layout assumed (set up by init script):
 *   /base   -- the underlying real directory
 *   /work   -- BranchFS mount of /base
 *
 * Build statically:  gcc -static -O2 -o test_branch test_branch.c
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define __NR_branch 470  /* vanilla v6.17 */

#define BR_CREATE 1
#define BR_COMMIT 2
#define BR_ABORT  3

#define BR_FS        (1u << 0)
#define BR_MEMORY    (1u << 1)
#define BR_ISOLATE   (1u << 2)
#define BR_CLOSE_FDS (1u << 3)

#define BR_NAME_MAX  128

union branch_attr {
	struct {
		uint32_t flags;
		int32_t  mount_fd;
		uint32_t n_branches;
		uint32_t __pad;
		uint64_t child_pids;	/* pid_t __user * */
		uint64_t branch_names;	/* char (__user *)[N][128] */
	} create;
	struct { uint32_t flags; int32_t ctl_fd; } commit;
	struct { uint32_t flags; int32_t ctl_fd; } abort_;
};

#define WORKDIR "/work"
#define BASEDIR "/base"

static long branch_call(int op, union branch_attr *attr, size_t size)
{
	return syscall(__NR_branch, op, attr, size);
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double t_start;

#define LOG(...) do { \
	printf("[%7.2fms] ", now_ms() - t_start); \
	printf(__VA_ARGS__); \
} while (0)

static int read_file(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY);
	int n;
	if (fd < 0) return -1;
	n = read(fd, buf, cap - 1);
	close(fd);
	if (n < 0) return -1;
	buf[n] = '\0';
	return n;
}

static int write_file(const char *path, const char *content)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	int len, n;
	if (fd < 0) return -1;
	len = strlen(content);
	n = write(fd, content, len);
	close(fd);
	return n == len ? 0 : -1;
}

static int file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

static void unlink_quiet(const char *path)
{
	(void)unlink(path);
}

static int open_ctl(const char *branch_name)
{
	char path[256];
	int fd;
	if (branch_name && branch_name[0]) {
		snprintf(path, sizeof(path), "%s/@%s/.branchfs_ctl",
		         WORKDIR, branch_name);
	} else {
		snprintf(path, sizeof(path), "%s/.branchfs_ctl", WORKDIR);
	}
	fd = open(path, O_RDONLY);
	if (fd < 0)
		fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));
	return fd;
}

static void chdir_branch(const char *branch_name)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/@%s", WORKDIR, branch_name);
	if (chdir(path) < 0) {
		fprintf(stderr, "chdir(%s) failed: %s\n", path, strerror(errno));
		_exit(10);
	}
}

/* ============================================================ *
 * Test 1: single-branch commit.
 * Child writes /work/@X/result.txt, commits. Parent verifies
 * the file lands in /base/result.txt.
 * ============================================================ */
static int test_single_commit(void)
{
	pid_t pid;
	pid_t pids[1] = {0};
	char names[1][BR_NAME_MAX] = {{0}};
	union branch_attr attr;
	int ctl_fd, status;
	double t0, t1, t2;
	long rv;
	char read_buf[64];

	printf("\n=========================================================\n");
	printf("Test 1: single-branch commit (N=1, BR_FS)\n");
	printf("=========================================================\n");

	unlink_quiet(BASEDIR "/result.txt");

	ctl_fd = open_ctl(NULL);
	if (ctl_fd < 0) return 1;

	memset(&attr, 0, sizeof(attr));
	attr.create.flags = BR_FS | BR_ISOLATE;
	attr.create.mount_fd = ctl_fd;
	attr.create.n_branches = 1;
	attr.create.child_pids = (uintptr_t)pids;
	attr.create.branch_names = (uintptr_t)names;

	t0 = now_ms();
	rv = branch_call(BR_CREATE, &attr, sizeof(attr));
	t1 = now_ms();
	if (rv < 0) {
		fprintf(stderr, "BR_CREATE failed: %s\n", strerror(errno));
		close(ctl_fd);
		return 1;
	}

	if (rv == 0) {
		LOG("parent: BR_CREATE took %.3fms; child=%d branch='%s'\n",
		    t1 - t0, pids[0], names[0]);
		close(ctl_fd);
		pid = waitpid(pids[0], &status, 0);
		LOG("parent: child %d %s=%d\n", pid,
		    WIFEXITED(status) ? "exit" : "sig",
		    WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));

		if (file_exists(BASEDIR "/result.txt") &&
		    read_file(BASEDIR "/result.txt", read_buf, sizeof(read_buf)) > 0) {
			LOG("VERIFY: %s exists, content='%s' %s\n",
			    BASEDIR "/result.txt", read_buf,
			    !strcmp(read_buf, "from-branch-1") ? "OK" : "MISMATCH");
			return strcmp(read_buf, "from-branch-1");
		}
		LOG("VERIFY FAIL: %s missing\n", BASEDIR "/result.txt");
		return 1;
	}

	/* child */
	{
		unsigned my_id = (unsigned)rv;
		const char *bname = names[my_id - 1];
		char path[256];
		int my_ctl;
		long crv;

		LOG("child: branch_id=%u name='%s'\n", my_id, bname);
		chdir_branch(bname);

		snprintf(path, sizeof(path), "%s/@%s/result.txt", WORKDIR, bname);
		if (write_file(path, "from-branch-1") < 0) {
			fprintf(stderr, "write %s failed: %s\n", path, strerror(errno));
			_exit(11);
		}
		LOG("child: wrote %s\n", path);

		my_ctl = open_ctl(bname);
		if (my_ctl < 0) _exit(12);

		memset(&attr, 0, sizeof(attr));
		attr.commit.ctl_fd = my_ctl;
		t0 = now_ms();
		crv = branch_call(BR_COMMIT, &attr, sizeof(attr));
		t2 = now_ms();
		close(my_ctl);

		if (crv == 0) {
			LOG("child: BR_COMMIT OK (%.3fms)\n", t2 - t0);
			_exit(0);
		}
		fprintf(stderr, "child: BR_COMMIT failed: %s\n", strerror(errno));
		_exit(13);
	}
}

/* ============================================================ *
 * Test 2: first-commit-wins (N=3).
 * Three children sleep different amounts, each tries to commit
 * with different content. Branch 1 wins (sleeps shortest).
 * Verify /base/race.txt has branch 1's content.
 * ============================================================ */
static int test_first_commit_wins(void)
{
	pid_t pids[3] = {0};
	char names[3][BR_NAME_MAX] = {{0}};
	union branch_attr attr;
	int ctl_fd;
	long rv;
	double t0, t1;
	char read_buf[64];
	int i, n_killed = 0, n_winner = 0, n_estale = 0;

	printf("\n=========================================================\n");
	printf("Test 2: first-commit-wins (N=3, BR_FS)\n");
	printf("=========================================================\n");

	unlink_quiet(BASEDIR "/race.txt");

	ctl_fd = open_ctl(NULL);
	if (ctl_fd < 0) return 1;

	memset(&attr, 0, sizeof(attr));
	attr.create.flags = BR_FS | BR_ISOLATE;
	attr.create.mount_fd = ctl_fd;
	attr.create.n_branches = 3;
	attr.create.child_pids = (uintptr_t)pids;
	attr.create.branch_names = (uintptr_t)names;

	t0 = now_ms();
	rv = branch_call(BR_CREATE, &attr, sizeof(attr));
	t1 = now_ms();
	if (rv < 0) {
		fprintf(stderr, "BR_CREATE failed: %s\n", strerror(errno));
		close(ctl_fd);
		return 1;
	}

	if (rv == 0) {
		LOG("parent: BR_CREATE(N=3) took %.3fms\n", t1 - t0);
		LOG("parent: branches: '%s' / '%s' / '%s'\n",
		    names[0], names[1], names[2]);
		close(ctl_fd);
		for (i = 0; i < 3; i++) {
			int status;
			pid_t r = waitpid(pids[i], &status, 0);
			if (WIFEXITED(status)) {
				int ec = WEXITSTATUS(status);
				if (ec == 0) n_winner++;
				else if (ec == 2) n_estale++;
				LOG("parent: child %d exit=%d\n", r, ec);
			} else if (WIFSIGNALED(status)) {
				if (WTERMSIG(status) == 9) n_killed++;
				LOG("parent: child %d sig=%d\n", r, WTERMSIG(status));
			}
		}
		LOG("parent: winner=%d estale=%d killed=%d\n",
		    n_winner, n_estale, n_killed);

		if (file_exists(BASEDIR "/race.txt") &&
		    read_file(BASEDIR "/race.txt", read_buf, sizeof(read_buf)) > 0) {
			LOG("VERIFY: %s = '%s' %s\n",
			    BASEDIR "/race.txt", read_buf,
			    !strcmp(read_buf, "winner-from-branch-1") ? "OK" : "MISMATCH");
			return strcmp(read_buf, "winner-from-branch-1");
		}
		LOG("VERIFY FAIL: race.txt missing in base\n");
		return 1;
	}

	/* child */
	{
		unsigned my_id = (unsigned)rv;
		const char *bname = names[my_id - 1];
		char path[256], content[64];
		struct timespec slp;
		int my_ctl;
		long crv;

		LOG("child: branch_id=%u name='%s'\n", my_id, bname);
		chdir_branch(bname);

		/* branch 1: 50ms, branch 2: 200ms, branch 3: 350ms */
		slp.tv_sec = 0;
		slp.tv_nsec = (50 + (my_id - 1) * 150) * 1000L * 1000L;
		nanosleep(&slp, NULL);

		snprintf(path, sizeof(path), "%s/@%s/race.txt", WORKDIR, bname);
		snprintf(content, sizeof(content), "winner-from-branch-%u", my_id);
		if (write_file(path, content) < 0) _exit(11);

		my_ctl = open_ctl(bname);
		if (my_ctl < 0) _exit(12);

		memset(&attr, 0, sizeof(attr));
		attr.commit.ctl_fd = my_ctl;
		crv = branch_call(BR_COMMIT, &attr, sizeof(attr));
		close(my_ctl);

		if (crv == 0) {
			LOG("child %u: COMMIT WON\n", my_id);
			_exit(0);
		}
		if (errno == ESTALE) {
			LOG("child %u: lost (ESTALE)\n", my_id);
			_exit(2);
		}
		LOG("child %u: COMMIT errno=%s\n", my_id, strerror(errno));
		_exit(13);
	}
}

/* ============================================================ *
 * Test 3: abort discards changes.
 * Child writes scratch.txt, aborts. Verify base does not have it.
 * ============================================================ */
static int test_abort(void)
{
	pid_t pids[1] = {0};
	char names[1][BR_NAME_MAX] = {{0}};
	union branch_attr attr;
	int ctl_fd, status;
	long rv;
	double t0, t1;
	pid_t r;

	printf("\n=========================================================\n");
	printf("Test 3: abort discards changes (N=1, BR_FS)\n");
	printf("=========================================================\n");

	unlink_quiet(BASEDIR "/scratch.txt");

	ctl_fd = open_ctl(NULL);
	if (ctl_fd < 0) return 1;

	memset(&attr, 0, sizeof(attr));
	attr.create.flags = BR_FS | BR_ISOLATE;
	attr.create.mount_fd = ctl_fd;
	attr.create.n_branches = 1;
	attr.create.child_pids = (uintptr_t)pids;
	attr.create.branch_names = (uintptr_t)names;

	rv = branch_call(BR_CREATE, &attr, sizeof(attr));
	if (rv < 0) { close(ctl_fd); return 1; }

	if (rv == 0) {
		close(ctl_fd);
		r = waitpid(pids[0], &status, 0);
		LOG("parent: child %d %s=%d\n", r,
		    WIFEXITED(status) ? "exit" : "sig",
		    WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));

		if (file_exists(BASEDIR "/scratch.txt")) {
			LOG("VERIFY FAIL: scratch.txt leaked into base!\n");
			return 1;
		}
		LOG("VERIFY: scratch.txt does NOT exist in base — OK\n");
		return 0;
	}

	/* child */
	{
		unsigned my_id = (unsigned)rv;
		const char *bname = names[my_id - 1];
		char path[256];
		int my_ctl;

		LOG("child: branch_id=%u name='%s'\n", my_id, bname);
		chdir_branch(bname);

		snprintf(path, sizeof(path), "%s/@%s/scratch.txt", WORKDIR, bname);
		if (write_file(path, "should-be-discarded") < 0) _exit(11);
		LOG("child: wrote %s\n", path);

		my_ctl = open_ctl(bname);
		if (my_ctl < 0) _exit(12);

		memset(&attr, 0, sizeof(attr));
		attr.abort_.ctl_fd = my_ctl;
		t0 = now_ms();
		rv = branch_call(BR_ABORT, &attr, sizeof(attr));
		t1 = now_ms();
		LOG("child: BR_ABORT returned %ld errno=%s (took %.3fms)\n",
		    rv, strerror(errno), t1 - t0);
		_exit(99);  /* should not reach */
	}
}

/* ============================================================ *
 * Test 4: latency micro-benchmarks (1 branch, repeated)
 * ============================================================ */
static int test_latency(int iters)
{
	double t_create_sum = 0;
	int i;

	printf("\n=========================================================\n");
	printf("Test 4: latency micro-bench (%d iters, N=1, BR_FS)\n", iters);
	printf("=========================================================\n");

	for (i = 0; i < iters; i++) {
		pid_t pids[1] = {0};
		char names[1][BR_NAME_MAX] = {{0}};
		union branch_attr attr;
		int ctl_fd, status;
		long rv;
		double t0, t1, ms;
		pid_t r;

		ctl_fd = open_ctl(NULL);
		if (ctl_fd < 0) return 1;

		memset(&attr, 0, sizeof(attr));
		attr.create.flags = BR_FS;
		attr.create.mount_fd = ctl_fd;
		attr.create.n_branches = 1;
		attr.create.child_pids = (uintptr_t)pids;
		attr.create.branch_names = (uintptr_t)names;

		t0 = now_ms();
		rv = branch_call(BR_CREATE, &attr, sizeof(attr));
		t1 = now_ms();
		if (rv < 0) { close(ctl_fd); return 1; }

		if (rv == 0) {
			ms = t1 - t0;
			t_create_sum += ms;
			close(ctl_fd);
			r = waitpid(pids[0], &status, 0);
			(void)r;
		} else {
			unsigned my_id = (unsigned)rv;
			const char *bname = names[my_id - 1];
			int my_ctl;
			my_ctl = open_ctl(bname);
			if (my_ctl < 0) _exit(1);
			memset(&attr, 0, sizeof(attr));

			if ((i & 1) == 0) {
				/* commit */
				attr.commit.ctl_fd = my_ctl;
				t0 = now_ms();
				rv = branch_call(BR_COMMIT, &attr, sizeof(attr));
				t1 = now_ms();
				close(my_ctl);
				/* report from child via a tmpfile? simpler: print and exit */
				printf("[lat] iter %d commit: %.3fms\n", i, t1 - t0);
				_exit(0);
			} else {
				attr.abort_.ctl_fd = my_ctl;
				t0 = now_ms();
				rv = branch_call(BR_ABORT, &attr, sizeof(attr));
				t1 = now_ms();
				close(my_ctl);
				printf("[lat] iter %d abort: %.3fms\n", i, t1 - t0);
				_exit(0);
			}
		}
	}

	printf("[lat] avg BR_CREATE (parent-side): %.3fms over %d iters\n",
	       t_create_sum / iters, iters);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "all";
	int iters = argc > 2 ? atoi(argv[2]) : 5;
	int rc = 0;

	t_start = now_ms();
	printf("test_branch starting; pid=%d\n", getpid());

	if (!strcmp(mode, "1")    || !strcmp(mode, "all")) rc |= test_single_commit() << 0;
	if (!strcmp(mode, "2")    || !strcmp(mode, "all")) rc |= test_first_commit_wins() << 1;
	if (!strcmp(mode, "3")    || !strcmp(mode, "all")) rc |= test_abort() << 2;
	if (!strcmp(mode, "lat")  || !strcmp(mode, "all")) rc |= test_latency(iters) << 3;

	printf("\n========== summary: rc=0x%x ==========\n", rc);
	return rc;
}
