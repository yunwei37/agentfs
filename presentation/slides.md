---
theme: academic
title: 'Fork, Explore, Commit: OS Primitives for Agentic Exploration'
info: |
  ## Fork, Explore, Commit: OS Primitives for Agentic Exploration
  Agentic OS Workshop, ASPLOS 2026
class: text-center
coverDate: Agentic OS Workshop, ASPLOS 2026
drawings:
  persist: false
transition: fade
mdc: true
layout: cover
colorSchema: light
---

<div class="text-center">

<div class="text-4xl leading-relaxed">Fork, Explore, Commit:<br/>OS Primitives for Agentic Exploration</div>

<div class="mt-8 text-xl">
Cong Wang¹, Yusheng Zheng²
</div>

<div class="text-sm opacity-80 mt-2">
¹Multikernel Technologies, Inc. · ²UC Santa Cruz
</div>

</div>

<div class="abs-br m-4 mb-8 flex flex-col items-end gap-3">
  <div class="flex items-center gap-4">
    <img src="/ucsc-logo.png" class="h-10" alt="UC Santa Cruz" />
  </div>
</div>

<!--
Good morning everyone. I'm Yusheng Zheng from UC Santa Cruz. Today I'll present "Fork, Explore, Commit: OS Primitives for Agentic Exploration," joint work with Cong Wang from Multikernel Technologies.

We'll show why AI agents need new OS primitives for parallel exploration, and present BranchFS and the branch() syscall as our solution.
-->

---

# Problem: Agents Explore, But Have Side Effects

<div class="grid grid-cols-2 gap-5 text-sm mt-1">

<div>

- AI agents (SWE-Agent, OpenHands, Claude Code) run shell commands, edit files, install packages → **irreversible side effects**
- Increasingly exploring **multiple paths in parallel**: Tree-of-Thoughts, Reflexion, Best-of-N
- Need to **isolate** each path, **commit** the winner, **discard** the rest

<div class="mt-3 p-2 rounded border-2 border-dashed border-red-400 text-sm">
<strong>Example:</strong> An agent tries 3 bug fixes simultaneously — only the one passing tests should be committed.
</div>

</div>

<div>

### What Do Agents Use Today?

| Approach | Limitation |
|----------|-----------|
| Git stashing | Misses untracked files |
| Temp directories | No atomic commit |
| Containers | Heavyweight, needs root |
| Per-file snapshots | Misses shell side effects |

<div class="mt-2 p-2 bg-red-50 rounded border border-red-300 text-xs">
<strong>None</strong> captures all FS modifications with atomic commit/rollback. No parallel branching, no sibling invalidation.
</div>

</div>

</div>

<!--
Modern AI agents take real actions — shell commands, file edits, package installs — producing irreversible side effects. And increasingly, they explore multiple solution paths in parallel.

The core challenge: each path modifies filesystem state and spawns processes. We need to isolate each path, commit the winner, and discard the rest.

What do agents use today? Git stashing misses untracked files. Temp directories have no atomic commit. Containers are heavyweight. Per-file snapshots miss shell side effects. None captures all filesystem modifications with atomic commit and rollback.
-->

---

# Six Requirements for Agentic Exploration

<div class="text-base mt-2">

| # | Requirement | Why |
|---|------------|-----|
| **R1** | **Isolated parallel execution** | Concurrent paths modify same files |
| **R2** | **Atomic commit + single-winner** | Apply winner's changes, invalidate siblings |
| **R3** | **Hierarchical nesting** | Tree-of-Thoughts explores sub-variants |
| **R4** | **Complete filesystem coverage** | Must capture *all* modifications, not just tracked files |
| **R5** | **Lightweight, unprivileged, portable** | Sub-ms creation, no root, any FS (ext4, XFS, NFS...) |
| **R6** | **Process coordination** | Reliable termination, sibling isolation |

</div>

<div class="mt-3 p-3 bg-yellow-50 rounded border border-yellow-300 text-base text-center">
No existing OS mechanism satisfies all six requirements.
</div>

<!--
We distill six requirements for agentic exploration.

R1: Isolated parallel execution — because concurrent paths may modify the same files.

R2: Atomic commit with single-winner resolution — the winning path's changes must be applied atomically, and all siblings invalidated.

R3: Hierarchical nesting — Tree-of-Thoughts and similar patterns explore hierarchically, so branches must be nestable.

R4: Complete filesystem coverage — not just tracked source files, but ALL modifications including build artifacts and installed packages.

R5: Lightweight, unprivileged, and portable — branch creation must be sub-millisecond, require no root, and work on any filesystem.

R6: Process coordination — reliable termination of all processes in a branch, with sibling isolation.

As we'll show next, no existing OS mechanism satisfies all six.
-->

---

# Why Existing Mechanisms Fall Short

<div class="grid grid-cols-2 gap-4 text-xs mt-1">

<div>

### Filesystem Branching

| Feature | OverlayFS | Btrfs/ZFS | DM-Snap | **BranchFS** |
|---------|:---------:|:---------:|:-------:|:------------:|
| Portable FS | ✓ | ✗ | ✓ | **✓** |
| Nested branches | ✗* | ✓ | ✗ | **✓** |
| Commit-to-parent | ✗ | ✗ | ✗ | **✓** |
| Sibling invalidation | ✗ | ✗ | ✗ | **✓** |
| No root | ✗ | ✗ | ✗ | **✓** |

</div>

<div>

### Process Management

| Feature | pgrp | cgroup | PID ns | **branch()** |
|---------|:----:|:------:|:------:|:------------:|
| Reliable termination | ✗ | ✓ | ✓ | **✓** |
| No escape | ✗ | ✓ | ✓ | **✓** |
| Sibling isolation | ✗ | ✗ | ✓ | **✓** |
| No setup / No root | ✓ | ✗ | ✗ | **✓** |
| No PID 1 overhead | ✓ | ✓ | ✗ | **✓** |

</div>

</div>

<div class="mt-2 text-xs">

- **OverlayFS**: no commit semantics, requires root. **Btrfs/ZFS**: FS-specific. **DM-Snap**: O(depth) latency
- **Process groups**: escapable via `setsid()`. **Cgroups**: need setup + root. **PID ns**: PID 1 overhead
- Composing these in **userspace** creates race windows and fragile error handling

</div>

<!--
Let's look at the gap. For filesystem branching: OverlayFS lacks commit semantics and requires root. Btrfs and ZFS are filesystem-specific. Device-mapper has O(depth) latency. Only BranchFS checks all boxes.

For process management: process groups are escapable. Cgroups need setup and root. PID namespaces have PID 1 overhead. And composing these in userspace creates race windows between steps — a process can fork between cgroup creation and migration.

Our proposed BranchFS and branch() syscall are the only solutions that satisfy all requirements.
-->

---

# Our Solution: The Branch Context

<div class="grid grid-cols-2 gap-4 text-sm mt-1">

<div>

### Definition & Lifecycle

A **branch context** = CoW filesystem view (Δᵢ) + confined process group

```
Fork ──► Explore ──► Commit (winner)
                 └──► Abort  (losers)
```

**Four Core Properties:**

1. **Frozen origin** — parent read-only; no merge conflicts
2. **Parallel isolated execution** — N contexts, fully isolated
3. **First-commit-wins** — siblings auto-invalidated
4. **Nestable** — sub-contexts form exploration tree

</div>

<div>

### Architecture

<img src="/fig-architecture.png" class="rounded shadow" style="max-height: 280px;" alt="Architecture" />

<div class="text-xs opacity-80 mt-1">
<strong>branch()</strong> = process coordination; <strong>BranchFS</strong> = filesystem CoW
</div>

</div>

</div>

<!--
Our solution is the branch context — a new OS abstraction. It encapsulates a copy-on-write filesystem view plus a confined process group. The lifecycle is Fork, Explore, then Commit or Abort.

Four properties define the semantics: frozen origin eliminates merge conflicts; parallel isolated execution enables speedup; first-commit-wins provides automatic resolution; and nestability supports hierarchical exploration.

The architecture has two components. The branch() syscall coordinates process creation and mount namespace isolation. BranchFS, a FUSE daemon, provides each branch with an isolated copy-on-write view. When a branch commits, its delta is applied atomically to the parent, and siblings are invalidated.
-->

---

# BranchFS: FUSE-Based Copy-on-Write

<div class="grid grid-cols-2 gap-5 text-base mt-1">

<div>

### File-Level CoW (~3,400 lines of Rust)

- First write → **copy entire file** to branch's delta layer
- Subsequent reads/writes served from **delta copy**
- Unmodified files resolved by walking the **branch chain**

### Branch Chain Resolution

1. Check current branch's delta
2. Walk ancestor branches in order
3. Fall back to base directory
4. **Tombstone markers** prevent deleted files from reappearing

</div>

<div>

### Key Design Properties

<div class="border-l-4 border-blue-500 pl-3 mb-2">

**O(1) branch creation** — just create a delta directory

</div>

<div class="border-l-4 border-green-500 pl-3 mb-2">

**No root privileges** — runs entirely as a FUSE daemon

</div>

<div class="border-l-4 border-orange-500 pl-3 mb-2">

**Portable** — works on ext4, XFS, NFS, any FS

</div>

### Commit & Abort

- **Commit**: copy delta to parent, increment **epoch counter** → invalidates all siblings
- **Abort**: discard delta layer → **near-zero cost**
- Cost proportional to **modification size**, not total FS size

</div>

</div>

<!--
BranchFS is implemented in about 3,400 lines of Rust using FUSE.

It uses file-level copy-on-write. The first time a file is modified on a branch, the entire file is copied to the branch's delta layer. After that, all reads and writes go to the delta copy. Unmodified files are resolved by walking the branch chain — first checking the current branch, then ancestors, then the base directory. Tombstone markers handle deletions so deleted files don't reappear from the base.

Three key design properties. First, O(1) branch creation — we just create a new delta directory. Second, no root privileges — it runs entirely as a userspace FUSE daemon. Third, portable — works on ext4, XFS, NFS, anything.

For commit, we copy the delta to the parent and increment an epoch counter, which invalidates all siblings. Abort just discards the delta layer at near-zero cost. Importantly, the cost is proportional to modification size, not total filesystem size.
-->

---

# BranchContext: Python Integration Library

<div class="grid grid-cols-2 gap-5 text-sm mt-1">

<div>

### 7 Exploration Patterns

| Pattern | Strategy |
|---------|----------|
| **Speculate** | Race N candidates, first success wins |
| **BestOfN** | Run N, commit highest-scoring |
| **Reflexion** | Sequential retry with feedback |
| **TreeOfThoughts** | Hierarchical nested branches |
| **BeamSearch** | Keep top-K at each depth |
| **Tournament** | Pairwise elimination via judge |
| **Cascaded** | Start with 1, fan out on failure |

</div>

<div>

### Usage Example

```python
ctx = BranchContext("/workspace")

# Best-of-N: try 3 fixes, commit best
results = ctx.best_of_n(
    n=3,
    task=lambda b: try_fix(b),
    score=lambda b: run_tests(b)
)
# Winner automatically committed
```

- Each pattern manages **branch lifecycle** + **process isolation** internally
- Agent code only supplies **per-branch task logic**

<div class="mt-2 p-2 bg-green-50 rounded border border-green-300 text-xs">
<strong>github.com/multikernel/branching</strong>
</div>

</div>

</div>

<!--
To make this practical for agent developers, we provide BranchContext — a Python library with seven ready-to-use exploration patterns.

Speculate races N candidates and commits the first success. BestOfN runs all N and commits the highest-scoring. Reflexion does sequential retry with failure feedback. TreeOfThoughts supports hierarchical exploration with nested branches. BeamSearch keeps the top-K branches alive at each depth. Tournament does pairwise elimination. And Cascaded starts with one branch and adaptively fans out on failure.

Here's a simple example: best-of-N. You create a BranchContext, call best_of_n with three branches, provide a task function and a scoring function. The library handles all the branching, isolation, and cleanup — the winner is automatically committed.

The key point is: agent developers just provide per-branch task logic. The library manages the entire branch lifecycle and process isolation internally.
-->

---

# The branch() Syscall

<div class="grid grid-cols-2 gap-5 text-sm mt-1">

<div>

### Why a Kernel Primitive?

Setting up cgroups + PID ns + mount ns + FS branches in **userspace**:

- **Multi-step** with race windows between steps
- A process can fork between cgroup creation and migration
- **Error-prone** cleanup on partial failure

<div class="mt-2 p-2 bg-blue-50 rounded border border-blue-300 text-xs">
<code>branch()</code> composes all atomically in a <strong>single call</strong> with kernel-side cleanup on failure.
</div>

### Three Operations

- `BR_CREATE` — fork N children, each in its own branch
- `BR_COMMIT` — apply FS changes, terminate siblings
- `BR_ABORT` — discard changes, terminate self

</div>

<div>

### Interface

```c
long branch(int op,
            union branch_attr *attr,
            size_t size);
```

### Composable Flags

| Flag | Effect |
|------|--------|
| `BR_FS` | Mount namespace + FS branch |
| `BR_MEMORY` | Page-table CoW |
| `BR_ISOLATE` | Signal/ptrace barriers |
| `BR_CLOSE_FDS` | Close inherited FDs |

- FS-agnostic: generic `FS_IOC_BRANCH_*` ioctls
- Adding a new branching FS = implement 3 ioctls

</div>

</div>

<!--
Now let me explain why we need a kernel primitive.

Setting up cgroups, PID namespaces, mount namespaces, and filesystem branches in userspace is a multi-step process with race windows. A process can fork between cgroup creation and migration. Error handling for partial failures is fragile and error-prone.

The branch() syscall composes all of this atomically in a single call, with kernel-side cleanup on failure. This follows the same rationale that motivated clone() over manual fork() plus unshare().

The interface is simple: three operations. BR_CREATE forks N children, each in its own branch. BR_COMMIT applies filesystem changes and terminates siblings. BR_ABORT discards changes and terminates the branch.

Four composable flags control the scope: BR_FS for mount namespace and filesystem branching, BR_MEMORY for page-table copy-on-write, BR_ISOLATE for signal and ptrace barriers, and BR_CLOSE_FDS to close inherited file descriptors.

Importantly, the syscall is filesystem-agnostic. It communicates with branching filesystems through three generic ioctls. Adding a new branching filesystem just means implementing these three ioctls — no changes to the syscall or VFS layer.
-->

---

# Preliminary Evaluation

<div class="grid grid-cols-3 gap-3 text-xs mt-1">

<div class="border-2 border-blue-400 rounded-lg p-3">

<div class="font-semibold text-blue-600 mb-1 text-sm">Branch Creation — O(1)</div>

| Base Size | Latency |
|:-:|:-:|
| 100 files | 292 μs |
| 1,000 files | 317 μs |
| 10,000 files | 310 μs |

Independent of base size.

</div>

<div class="border-2 border-green-400 rounded-lg p-3">

<div class="font-semibold text-green-600 mb-1 text-sm">Commit & Abort</div>

| Mod. Size | Commit | Abort |
|:-:|:-:|:-:|
| 1 KB | 317 μs | 315 μs |
| 100 KB | 514 μs | 365 μs |
| 1 MB | 2.1 ms | 890 μs |

Proportional to mod size.

</div>

<div class="border-2 border-purple-400 rounded-lg p-3">

<div class="font-semibold text-purple-600 mb-1 text-sm">I/O Throughput</div>

| Mode | Read |
|:-:|:-:|
| Native | 8.8 GB/s |
| FUSE | 1.7 GB/s |
| Passthrough | **7.2 GB/s** |

82% native with passthrough.

</div>

</div>

<div class="mt-2 text-sm text-center">
Agent workloads dominated by <strong>LLM API latency</strong> (100 ms – 10 s) — BranchFS I/O overhead is <strong>negligible</strong>.
</div>

<div class="mt-1 text-xs text-center opacity-60">
Hardware: AMD Ryzen 5 5500U, 8 GB DDR4, NVMe SSD. Median of 10 trials. BranchFS: ~3,400 lines of Rust, FUSE 3.
</div>

<!--
For evaluation, branch creation is O(1) — under 350 microseconds regardless of base size, because it just allocates a delta directory.

Commit cost is proportional to modification size: under 1 millisecond for small changes, about 2 milliseconds for 1 megabyte. Abort is even faster.

For I/O throughput, with FUSE passthrough mode we achieve 7.2 gigabytes per second read — 82% of native. Write actually exceeds native because we treat fsync as a no-op for ephemeral branches.

Most importantly, agent workloads are dominated by LLM API latency of 100ms to 10 seconds, so BranchFS overhead is negligible in practice.
-->

---

# Related Work & Future Directions

<div class="grid grid-cols-2 gap-4 text-xs mt-1">

<div>

### Related Work

- **Containers/gVisor/Dune**: general containment, not branching
- **Speculator**: sequential speculation for distributed FS — we do parallel N-way with first-commit-wins
- **TxOS**: flat, short-lived OS transactions — we're nestable, long-lived, userspace FUSE
- **lwCs/Capsicum**: different isolation granularity

### Current Limitations

- **External side effects** (network, IPC) not rolled back
- **Single-winner** only — no multi-branch merge
- File-level CoW: symlinks, hardlinks unsupported
- **BR_MEMORY** not yet implemented

</div>

<div>

### Future Directions

<div class="border-l-4 border-blue-500 pl-3 mb-2">

**Effect gating** — buffer network/IPC until commit; agent gateways provide interposition points

</div>

<div class="border-l-4 border-green-500 pl-3 mb-2">

**Multi-branch merge** — union non-overlapping changes; conflict detection

</div>

<div class="border-l-4 border-purple-500 pl-3 mb-2">

**Roadmap** — prototype `branch()` on Linux 6.19; BR_FS + BR_ISOLATE first

</div>

<div class="border-l-4 border-orange-500 pl-3">

**Broader use** — `n_branches=1` → try-and-rollback for package mgmt, system tuning

</div>

</div>

</div>

<!--
Let me position this against related work and discuss future directions.

Containers and VMs provide general containment but not branching. Speculator pioneered OS speculation but is sequential and owner-decided — we do parallel N-way with competitive first-commit-wins. TxOS has flat, short-lived transactions requiring deep kernel mods — ours are nestable and run in userspace.

For limitations: we currently only isolate filesystem state, not network or IPC. We support single-winner only. The branch() syscall is still a design proposal.

Key future directions: effect gating to buffer external actions until commit, multi-branch merge, and prototyping branch() on Linux 6.19. Beyond agents, with n_branches=1 this provides try-and-rollback for package management and system tuning.
-->

---

# Key Takeaways

<div class="text-lg leading-relaxed mt-4">

1. **Agentic exploration** is a first-class OS concern: agents need isolated, parallel workspaces with atomic commit/rollback

2. **Branch context** = new OS abstraction with fork/explore/commit lifecycle and first-commit-wins resolution

3. **BranchFS** = working FUSE implementation: O(1) creation, no root, portable across filesystems

4. **BranchContext** = Python library with 7 ready-to-use exploration patterns

5. **branch() syscall** = proposed kernel primitive for atomic process + FS coordination

</div>

<div class="mt-4 flex gap-6 justify-center text-sm">

<div class="p-2 bg-blue-50 rounded border border-blue-300">
BranchFS: <strong>github.com/multikernel/branchfs</strong>
</div>

<div class="p-2 bg-green-50 rounded border border-green-300">
BranchContext: <strong>github.com/multikernel/branching</strong>
</div>

</div>

<!--
To summarize our five key takeaways.

First, agentic exploration is a first-class OS concern. AI agents need isolated, parallel workspaces with atomic commit and rollback semantics.

Second, the branch context is a new OS abstraction with a fork-explore-commit lifecycle and first-commit-wins resolution.

Third, BranchFS is a working FUSE implementation — you can use it today. It provides O(1) creation, requires no root privileges, and is portable across filesystems.

Fourth, BranchContext is a Python library providing seven ready-to-use exploration patterns that wrap BranchFS into a high-level API.

Fifth, the branch() syscall is our proposed kernel primitive for atomic composition of process and filesystem coordination.

Both BranchFS and BranchContext are open source — please check them out on GitHub.
-->

---
layout: center
class: text-center
---

# Thank You — Questions?

<div class="mt-6 text-lg">

**Cong Wang** — cwang@multikernel.io

**Yusheng Zheng** — yzhen165@ucsc.edu

</div>

<div class="mt-4 text-base opacity-70">
Agentic OS Workshop, ASPLOS 2026
</div>

<div class="mt-3 flex gap-6 justify-center text-sm">

<div>BranchFS: github.com/multikernel/branchfs</div>
<div>BranchContext: github.com/multikernel/branching</div>

</div>

<!--
Thank you for your attention. I'm happy to take any questions. Feel free to also reach out via email, or check out the code on GitHub.
-->
