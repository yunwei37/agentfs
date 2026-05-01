# Test results

End-to-end results for the prototype, captured from a QEMU/KVM boot
of a vanilla Linux v6.17 kernel patched with the three commits in
`patches/`. Full boot log: `results/qemu-run.log`.

Host: AMD EPYC, 24 cores, 128 GB RAM, KVM accelerated. Guest: 4 vCPU,
2 GB RAM, no devices except virtio. The base directory `/base` is
empty at the start of each test (only contains files from prior
committed branches).

## Functional tests

```
test_branch starting; pid=88

=========================================================
Test 1: single-branch commit (N=1, BR_FS)
=========================================================
[   0.25ms] parent: BR_CREATE took 0.087ms; child=89 branch='branch-7a9da7e0-…'
[   0.28ms] child: branch_id=1 name='branch-7a9da7e0-…'
[   0.53ms] child: wrote /work/@branch-7a9da7e0-…/result.txt
[   0.70ms] child: BR_COMMIT OK (0.088ms)
[   0.76ms] parent: child 89 exit=0
[   0.78ms] VERIFY: /base/result.txt exists, content='from-branch-1' OK

=========================================================
Test 2: first-commit-wins (N=3, BR_FS)
=========================================================
[   1.13ms] parent: BR_CREATE(N=3) took 0.192ms
[   1.17ms] parent: branches: 'branch-7c95251c-…' / 'branch-3e1a6a9c-…' / 'branch-c2c97c6b-…'
[  54.78ms] child 1: COMMIT WON
[  55.10ms] parent: child 90 exit=0
[  55.12ms] parent: child 91 sig=9
[  55.17ms] parent: child 92 sig=9
[  55.23ms] parent: winner=1 estale=0 killed=2
[  55.33ms] VERIFY: /base/race.txt = 'winner-from-branch-1' OK

=========================================================
Test 3: abort discards changes (N=1, BR_FS)
=========================================================
[  55.67ms] child: branch_id=1 name='branch-b992aa6c-…'
[  56.16ms] child: wrote /work/@branch-b992aa6c-…/scratch.txt
[  56.30ms] parent: child 93 exit=0
[  56.33ms] VERIFY: scratch.txt does NOT exist in base — OK
```

| # | Lifecycle exercised | What is verified | Outcome |
|---|---------------------|------------------|---------|
| 1 | `BR_CREATE` (N=1) → child writes file in branch view → `BR_COMMIT` | `/base/result.txt` contains `"from-branch-1"` (FS commit reached base) | ✅ |
| 2 | `BR_CREATE` (N=3) → siblings race; branch 1 sleeps 50 ms; 2 sleeps 200 ms; 3 sleeps 350 ms; all try `BR_COMMIT` | branch 1 commits; branches 2 & 3 die with `sig=9` (SIGKILL); `/base/race.txt = "winner-from-branch-1"` | ✅ |
| 3 | `BR_CREATE` (N=1) → child writes file → `BR_ABORT` | `/base/scratch.txt` does **not** exist | ✅ |

`========== summary: rc=0x0 ==========` confirms all functional checks
passed. The kernel boot log shows no warnings, oopses, or stack traces
in any of the relevant subsystems (branch, fork, exit, sched, fuse).

## Latency micro-benchmark (Test 4)

50 iterations, N=1, `BR_FS`, alternating commit and abort:

| Operation | What's measured | Mean | Notes |
|-----------|-----------------|------|-------|
| `BR_CREATE` (parent side) | Round-trip including 1× `vfs_ioctl(FS_IOC_BRANCH_CREATE)` to BranchFS via FUSE + 1× `kernel_clone()` | **61–70 μs** (avg over 50 iters across two runs) | dominated by FUSE round-trip and process creation |
| `BR_COMMIT` (child side) | Round-trip including atomic CAS + `vfs_ioctl(FS_IOC_BRANCH_COMMIT)` via FUSE + sibling cleanup (none in N=1) | **12–25 μs** (steady-state, after first iteration) | first iter ~25–40 μs (cold cache) |

Sample steady-state output:

```
[lat] iter 20 commit: 0.014ms
[lat] iter 22 commit: 0.013ms
[lat] iter 30 commit: 0.013ms
[lat] iter 32 commit: 0.014ms
[lat] iter 38 commit: 0.014ms
[lat] avg BR_CREATE (parent-side): 0.061ms over 50 iters
```

## How these compare to the paper

The paper reports BranchFS branch creation at **<350 μs** (independent
of base FS size, Table 1). Our measurements are roughly an order of
magnitude faster:

- We measure the **kernel syscall** round-trip, not BranchFS daemon
  internals; the daemon work is small relative to the FUSE crossing.
- Our base directory is empty (test fixture). The paper's measurement
  was already O(1) in base size.
- KVM virtualization introduces some FUSE roundtrip overhead but our
  numbers are still well below the bound.

These numbers are *not* a paper-quality benchmark — they're a sanity
check that the prototype isn't doing something pathological. A
publishable benchmark would need:

- Multiple base sizes, modification volumes, and N values.
- Bare-metal runs (no QEMU).
- p50 / p99 distributions over 1000s of iterations.
- Comparison against `unshare` + `overlayfs` baseline.

## Reproducing

`scripts/build-kernel.sh && (cd test && make) && BRANCHFS_BIN=… TEST_BIN=test/test_branch scripts/build-rootfs.sh && scripts/run-qemu.sh`

Total wall time on a 24-core host: **~5 minutes** (3 min kernel build,
20 s BranchFS, 1 s test program, ~30 s initramfs, 1 s QEMU boot + tests).
