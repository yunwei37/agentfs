# Kernel patches

Three logical commits against vanilla Linux **v6.17**. Generated with
`git format-patch v6.17..branch-syscall`.

```
0001-branch-add-UAPI-and-kernel-internal-headers.patch     +117  lines
0002-branch-implement-syscall-and-wire-ioctls-to-branchin.patch +482  lines
0003-branch-hook-copy_process-and-do_exit-add-task_struct.patch  +19  lines
                                                          —————————
total                                                      +618 lines
```

## Apply

```bash
cd linux-v6.17
git checkout -b branch-syscall v6.17
git am /path/to/prototype/patches/*.patch
```

`scripts/build-kernel.sh` does this automatically against a fresh
`linux-vanilla/` checkout.

## checkpatch status

`scripts/checkpatch.pl --no-tree --no-signoff` reports zero errors and
one warning per patch — `MAINTAINERS may need updating`, which is
expected for new files in a real upstream submission and not relevant
to this prototype.

## Why three commits?

- **0001** introduces *only* type definitions and constants
  (`include/uapi/linux/branch.h`, `include/linux/branch.h`). On its own
  this commit is a no-op — nothing references the new types yet.
- **0002** adds the syscall implementation, allocates a syscall number,
  and wires it into the kernel build. Reviewable in isolation; any
  reviewer can read `kernel/branch.c` against the headers from 0001
  without context-switching.
- **0003** is the only commit that touches existing core kernel paths
  (`copy_process` and `do_exit`). Keeping it separate makes the impact
  on hot paths obvious in `git log`. With `CONFIG_BRANCH=n` the hooks
  are static-inline no-ops, so this commit also has zero behavioural
  change for users who don't opt in.

## What the patches *do not* touch

- `MAINTAINERS` — would be updated for upstream submission.
- `Documentation/` — same; the design lives in the paper.
- Other architectures' syscall tables — `branch()` is allocated at #470
  on x86_64 only. Per-arch enabling is straightforward but not done
  here.

## Verification

After applying these three patches and configuring a kernel with
`CONFIG_BRANCH=y` (default y) and `CONFIG_FUSE_FS=y`, the kernel
builds clean (`make -j$(nproc)`) and boots in QEMU. End-to-end
behaviour is verified by the test driver in `../test/test_branch.c`;
captured output in `../results/qemu-run.log`.
