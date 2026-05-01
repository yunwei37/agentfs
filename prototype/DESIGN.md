# `branch()` prototype — design notes

Companion to `README.md`. This document explains *why* the prototype is
structured the way it is, and the contract between the kernel syscall
and the userspace branching filesystem (BranchFS).

## Syscall ABI

```c
long branch(int op, union branch_attr *attr, size_t size);
```

`op` selects an operation; `attr` points to a per-op argument struct
inside a `union`; `size` is `sizeof(union branch_attr)` so the kernel
can extend the union later without breaking ABI (same trick as
`bpf(2)` and `perf_event_open(2)`).

Three operations:

| op          | value | who calls it | semantics                                      |
|-------------|-------|--------------|------------------------------------------------|
| `BR_CREATE` | 1     | parent       | create N branches, fork N children             |
| `BR_COMMIT` | 2     | child        | commit this branch to the parent FS, then exit |
| `BR_ABORT`  | 3     | child        | discard this branch and exit                   |

The full UAPI is in `patches/0001-…`, `include/uapi/linux/branch.h`.

## How the kernel and BranchFS talk

The paper's §4.4 prescribes that the syscall is filesystem-agnostic:
all FS work goes through three generic ioctls.

```
FS_IOC_BRANCH_CREATE  _IOR('b', 0, char[128])  →  0x80806200
FS_IOC_BRANCH_COMMIT  _IO ('b', 1)             →  0x6201
FS_IOC_BRANCH_ABORT   _IO ('b', 2)             →  0x6202
```

These numbers were chosen to be byte-identical to the constants
hard-coded in `branchfs/src/platform/linux.rs`, so the prototype works
without patching either side. This was verified by compiling a small
program (in commit messages of `patches/0001-…`) that prints the three
values from `<linux/ioctl.h>` macros.

### Flow on `BR_CREATE`

1. Userspace mounts BranchFS at `/work` with base `/base` and opens
   `/work/.branchfs_ctl` as a file descriptor.
2. Userspace calls `branch(BR_CREATE, {flags=BR_FS, mount_fd=ctl_fd,
   n_branches=N, child_pids=…, branch_names=…})`.
3. Kernel, for `i = 0..N`:
   - Calls `vfs_ioctl(mount_fd_file, FS_IOC_BRANCH_CREATE,
     (unsigned long)&user_branch_names[i])`.
   - FUSE forwards this to the BranchFS daemon, which allocates a fresh
     branch (a UUID-named subdirectory) and writes the 128-byte name
     back via `copy_to_user` to the user buffer.
   - Kernel reads the name back kernel-side and stashes it in
     `branch_member[i]->name` for diagnostic logging.
4. Kernel then forks N children. Each child's `pt_regs->ax` is set to
   its `branch_id` so it returns from `branch(BR_CREATE)` with that
   value (`fork_inherit` hook in `copy_process`).
5. Userspace children read their assigned branch name from the
   userspace `branch_names[branch_id - 1]` slot (the parent's
   buffer is inherited by COW).

### Flow on `BR_COMMIT`

1. Child, having done its work in `/work/@<name>/`, opens
   `/work/@<name>/.branchfs_ctl` (a *per-branch* control fd; BranchFS
   identifies the target branch by the inode of the open fd).
2. Child calls `branch(BR_COMMIT, {flags=0, ctl_fd=…})`.
3. Kernel does `atomic_try_cmpxchg(&group->winner, 0, branch_id)`:
   - If it loses the CAS: returns `-ESTALE`. No FS work, no termination.
   - If it wins: continues to step 4.
4. Kernel calls `vfs_ioctl(ctl_file, FS_IOC_BRANCH_COMMIT, 0)`. The
   BranchFS daemon merges the branch's delta into the parent branch
   (or into the base for top-level branches) and increments its epoch.
5. Kernel snapshots sibling task pointers under the group spinlock,
   pins them via `get_task_struct()`, drops the lock, and SIGKILLs
   them outside the lock to avoid `sighand->siglock` lock-ordering
   issues.
6. Kernel detaches the winner from its branch_member, frees it, and
   returns 0 to the winner.

### Flow on `BR_ABORT`

1. Child calls `branch(BR_ABORT, {flags=0, ctl_fd=…})`.
2. Kernel `vfs_ioctl(ctl_file, FS_IOC_BRANCH_ABORT, 0)` → BranchFS
   discards the branch's delta layer.
3. Kernel calls `do_exit(0)`. The exit path's `branch_exit()` hook
   releases the `branch_member`. If this was the last member, the
   `branch_group` is freed (via `refcount_dec_and_test`).

## Kernel-side data structures

```
struct branch_group {                struct branch_member {
    refcount_t  refcount;                struct branch_group *group;
    atomic_t    winner;       ←CAS       struct task_struct  *task;
    u32         n_branches;              u32  branch_id;          /* 1..N */
    u32         flags;                   bool aborted;
    spinlock_t  lock;                    struct list_head node;
    struct list_head members;            char name[BR_NAME_MAX];  /* FS-assigned */
    struct task_struct *parent;     };
};
```

A `branch_group` is shared by parent and all N children of a single
`BR_CREATE` call. Each child owns one `branch_member` reachable via
`task->branch`. The parent transfers ownership to children at fork
time using a separate `task->branch_pending` slot; this avoids racing
the child to assign its membership.

### Why `branch_pending`

`copy_process()` runs in the parent's context but produces a child
`task_struct`. We can't simply assign `child->branch = m` after
`kernel_clone()` returns because the child may already be running.
Instead the parent stores the staged member in
`current->branch_pending` *before* `kernel_clone()`; `copy_process()`
calls our `branch_fork_inherit(p)` hook between `copy_thread()` and
`wake_up_new_task()`, where the child is created but not yet runnable.
The hook moves the pointer:

```
p->branch         = current->branch_pending;
p->branch_pending = NULL;
m->task           = p;
task_pt_regs(p)->ax = m->branch_id;   /* override syscall return value */
```

The parent clears `branch_pending` after each `kernel_clone()`, so the
field is set for exactly one fork at a time.

### Why `do_exit` hook

A child can leave the branch group by three paths:

1. Explicit `BR_COMMIT` (winner) — frees its own member directly.
2. Explicit `BR_ABORT` — frees via `do_exit` → `branch_exit()`.
3. SIGKILL from the winner — same path as (2).

We also need a defensive cleanup if the child crashes mid-work (segfault,
oops, OOM kill). All of these go through `do_exit`, so hooking it once
covers everything.

## What's deliberately *not* in the kernel

The paper's full syscall design also covers:

- **`BR_MEMORY`** — page-table COW. Requires walking the parent's PTEs
  and replacing them with read-only entries that fault into per-branch
  copies. Substantial mm work; the paper itself flags this as
  "follow-on effort due to the page table complexity involved". The
  prototype rejects this flag with `-EOPNOTSUPP`.
- **Mount-namespace setup** — the paper proposes that each child
  receives a private mount namespace with its `@<name>/` bind-mounted
  over the original `mount_fd` path, so the child sees `/work` as its
  branch transparently. Without this, children must `chdir("/work/@<name>")`
  themselves. The bind-mount path uses the new mount API
  (`open_tree` + `move_mount`) which is itself non-trivial to drive
  from a syscall. Punted to follow-up.
- **`BR_ISOLATE`** — kernel-enforced fence preventing siblings from
  signalling or ptracing each other. The flag is accepted today but
  the fence isn't plumbed into the signal/ptrace paths yet. It is
  implementable in a few hundred lines but requires touching
  `kernel/signal.c` and `kernel/ptrace.c`.

## Testing strategy

The prototype is verified end-to-end inside QEMU/KVM:

1. **T1 single commit** — N=1 child writes `result.txt` via the branch's
   FUSE view, commits, parent verifies the file lands at the *base*
   directory. This proves the `vfs_ioctl(FS_IOC_BRANCH_COMMIT)` path
   actually merges the delta layer.
2. **T2 first-commit-wins** — N=3 children with different sleep
   durations. Verifies the kernel's CAS, sibling SIGKILL, and that the
   *winner's* file (with content `winner-from-branch-1`) is what
   appears in the base.
3. **T3 abort discards** — N=1 child writes `scratch.txt`, calls
   `BR_ABORT`. Verifies the file does *not* appear in the base.
4. **T4 latency** — 50 iterations alternating commit/abort to measure
   syscall round-trip cost.

Each test prints `OK` lines that the test driver aggregates into a
return code. `rc=0x0` means all four tests passed.

## Limitations as a prototype

These are choices made for prototype simplicity; none is fundamental.

- **Single architecture (x86_64)**. Other arches need their own
  `pt_regs` accessor for the syscall-return override (one line each).
- **No SMP stress tests** in the test driver. Parent and 3 children on
  4 vCPUs is the largest configuration tested.
- **No fault injection**. The paper's full design has well-defined
  behaviour on partial failure of `BR_CREATE` (e.g. FS error after
  some children forked). The prototype kills already-forked children
  on FS-create failure but does not exhaustively test those paths.
- **Hardcoded to BranchFS layout**. `chdir("/work/@<name>")` and
  opening `/work/@<name>/.branchfs_ctl` are conventions BranchFS
  defines. A different branching FS would need its own conventions or
  the kernel-side mount-namespace setup.
