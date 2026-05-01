# `branch()` syscall prototype

Prototype implementation of the `branch()` syscall proposed in the paper
**Fork, Explore, Commit: OS Primitives for Agentic Exploration**
(ASPLOS 2026 Agentic OS Workshop) — `../main.tex`, §"The branch() Syscall".

This is a *working* prototype, not a final design. It boots a vanilla
Linux v6.17 kernel patched with the new syscall, mounts BranchFS over a
real directory, and runs end-to-end tests that prove file-level
`commit` / `abort` semantics work against the underlying filesystem.

## What's here

```
prototype/
├── README.md              you are here
├── DESIGN.md              kernel/userspace contract + design notes
├── RESULTS.md             test results and latency numbers
├── patches/               git format-patch series against vanilla v6.17
│   ├── 0001-branch-add-UAPI-and-kernel-internal-headers.patch
│   ├── 0002-branch-implement-syscall-and-wire-ioctls-to-branchin.patch
│   └── 0003-branch-hook-copy_process-and-do_exit-add-task_struct.patch
├── test/
│   ├── test_branch.c      static C driver: 4 tests across the lifecycle
│   └── Makefile
├── scripts/
│   ├── build-kernel.sh    clone v6.17, apply patches, build bzImage
│   ├── build-rootfs.sh    pack busybox + branchfs + libfuse + test into cpio
│   ├── run-qemu.sh        boot under QEMU/KVM, run tests, power off
│   └── init.sh            initramfs /init that mounts BranchFS and runs tests
└── results/
    └── qemu-run.log       captured boot + test output (rc=0x0, all green)
```

## What this does *not* contain

- The full Linux kernel source tree — fetch via the build script.
- The BranchFS source — clone from `https://github.com/multikernel/branchfs`.
- Compiled binaries (`bzImage`, `initramfs.cpio.gz`, `test_branch`) — produced
  by the build scripts and ignored via `prototype/.gitignore`.

## Quick start

Prerequisites on the host:

```bash
sudo apt install build-essential libncurses-dev bison flex libssl-dev libelf-dev \
                 bc qemu-system-x86 cpio busybox-static \
                 libfuse2t64 fuse rustc cargo git
```

Build & run (about 5 minutes on a 24-core machine):

```bash
# 1. fetch v6.17, apply patches, build kernel
./scripts/build-kernel.sh

# 2. build BranchFS (the FUSE filesystem)
git clone https://github.com/multikernel/branchfs ../../branchfs
cargo --manifest-path ../../branchfs/Cargo.toml build --release

# 3. build the test program
make -C test

# 4. assemble initramfs (needs the BranchFS binary and the test binary)
BRANCHFS_BIN=../../branchfs/target/release/branchfs \
TEST_BIN=test/test_branch \
    ./scripts/build-rootfs.sh

# 5. boot under QEMU and run tests
./scripts/run-qemu.sh
```

Expected output ends with:

```
========== summary: rc=0x0 ==========
```

`rc=0x0` means: single-branch commit verified, three-way first-commit-wins
verified, abort-discards-changes verified, latency micro-bench done.

## Reading order

1. `DESIGN.md` — what the syscall looks like, how it talks to the FS.
2. `RESULTS.md` — what the tests verify and the measured numbers.
3. `patches/` — the actual code, three logical commits.
4. `test/test_branch.c` — the userspace driver, mirrors the paper's
   §4.2 listing.

## Status vs the paper

| Capability                         | Status |
|-----------------------------------|--------|
| `BR_CREATE` / `BR_COMMIT` / `BR_ABORT` | ✅ implemented |
| First-commit-wins (kernel CAS)     | ✅ implemented |
| Sibling termination via `SIGKILL`  | ✅ implemented |
| `BR_FS` ↔ `FS_IOC_BRANCH_*` ioctls | ✅ implemented, wired to BranchFS |
| Per-task `branch_id` return value  | ✅ implemented (via `pt_regs->ax`) |
| `BR_ISOLATE` (signal/ptrace fence) | ⚠️  flag accepted, kernel-enforced fence is not yet plumbed |
| Mount-namespace + bind-mount setup | ❌ deferred — children must `chdir(@<name>)` |
| `BR_MEMORY` (page-table COW)       | ❌ deferred — returns `-EOPNOTSUPP` |
| Nested branches                    | ❌ deferred — `current->branch != NULL` returns `-EBUSY` |

The deferred items are explicitly called out in the paper's
§"Implementation Roadmap" as follow-on work.
