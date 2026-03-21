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

# The Problem: Agents Have Side Effects

<div class="grid grid-cols-2 gap-6 text-base mt-2">

<div class="border-l-4 border-blue-500 pl-4">

### Agentic Exploration

- AI agents (SWE-Agent, OpenHands, Claude Code) execute **multi-step tasks**
- Shell commands, file edits, package installs → **irreversible side effects**
- Increasingly exploring **multiple paths in parallel**:
  Tree-of-Thoughts, Reflexion, Best-of-N

</div>

<div class="border-l-4 border-orange-500 pl-4">

### The Core Challenge

- Each path modifies **filesystem state** (workspace files)
- Each path spawns **processes** (compilers, test runners)
- Need to **isolate** each path, **commit** the winner, **discard** the rest

</div>

</div>

<div class="mt-4 p-3 rounded border-2 border-dashed border-red-400 text-base text-center">
<strong>Example:</strong> A software engineering agent tries 3 candidate fixes simultaneously.<br/>Only the one that passes tests should be committed — the rest must be cleanly discarded.
</div>

<!--
Modern AI agents don't just chat — they take real actions. Software engineering agents like SWE-Agent and OpenHands run shell commands, edit files, install packages. Each of these actions produces irreversible side effects on the filesystem.

And increasingly, agent systems explore multiple solution paths in parallel — using strategies like Tree-of-Thoughts, Reflexion, or Best-of-N sampling.

The core challenge is: each exploration path modifies filesystem state and spawns processes. We need a way to isolate each path, commit the successful one, and cleanly discard the rest.

For example, imagine a software engineering agent trying three different bug fixes simultaneously. Only the one that passes all tests should be committed. The other two must be discarded without leaving any trace.
-->

---

# What Do Agents Use Today?

<div class="text-base mt-2">

| Approach | Limitation |
|----------|-----------|
| **Git stashing** | Cannot capture non-tracked files (build artifacts, node_modules) |
| **Temporary directories** | Manual setup/teardown, no atomic commit |
| **Container clones** | Heavyweight, requires root, slow startup |
| **Per-file snapshots** | Miss shell command side effects, no parallel branching |

</div>

<div class="mt-4 p-3 bg-red-50 rounded border border-red-300 text-base text-center">
<strong>None</strong> captures all filesystem modifications with atomic commit/rollback.
</div>

<div class="mt-3 text-sm opacity-80">

- **LangChain & AutoGPT**: rely on git stashing or temporary directories
- **Claude Code**: per-file snapshots that cannot capture `npm install` side effects

</div>

<!--
So what do agents use today? Let me walk through the options.

Git stashing is popular, but it only captures tracked files. Build artifacts, node_modules, dotfiles — all missed.

Temporary directories require manual setup and teardown, with no atomic commit semantics.

Container clones are heavyweight — they require root privileges and have slow startup times.

Per-file snapshots, like what Claude Code uses, miss side effects from shell commands like npm install, and don't support parallel branching.

The bottom line: none of these approaches captures all filesystem modifications with atomic commit and rollback semantics. This is the gap we're addressing.
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

# Gap: Existing Filesystem Mechanisms

<div class="text-base mt-2">

| Feature | OverlayFS | Btrfs/ZFS | DM-Snap | DAXFS | **BranchFS** |
|---------|:---------:|:---------:|:-------:|:-----:|:------------:|
| Portable across filesystems | ✓ | ✗ | ✓ | ✗ | **✓** |
| Nested branches | ✗* | ✓ | ✗ | ✓ | **✓** |
| Commit-to-parent | ✗ | ✗ | ✗ | ✓ | **✓** |
| Sibling invalidation | ✗ | ✗ | ✗ | ✓ | **✓** |
| No root privileges | ✗ | ✗ | ✗ | ✗ | **✓** |

</div>

<div class="mt-3 text-sm">

- **OverlayFS**: stacked overlays since Linux 5.0, but no commit semantics, requires root
- **Btrfs/ZFS**: nested subvolumes, but filesystem-specific, no commit-to-parent
- **Device-mapper snapshots**: require raw block devices, O(depth) read latency
- **DAXFS**: branching with commit, but memory-backed only — not portable

</div>

<!--
Let's look at the gap in filesystem mechanisms.

OverlayFS supports stacked overlays since Linux 5.0, but it has no native commit-to-parent or sibling invalidation, and mounting requires root.

Btrfs and ZFS support nested subvolumes and clones, but they're filesystem-specific — you can't use them on ext4 or NFS. And they lack commit-to-parent semantics.

Device-mapper snapshots require raw block devices and have O(depth) read latency that scales poorly with nesting.

DAXFS is the closest — it provides branching with commit and sibling invalidation — but it targets memory-backed storage exclusively and isn't portable.

Only our BranchFS provides all five properties: portable, nested, commit-to-parent, sibling invalidation, and no root privileges.
-->

---

# Gap: Existing Process Management

<div class="text-base mt-2">

| Feature | pgrp | session | cgroup | PID ns | **branch()** |
|---------|:----:|:-------:|:------:|:------:|:------------:|
| Reliable termination | ✗ | ✗ | ✓ | ✓ | **✓** |
| No escape possible | ✗ | ✗ | ✓ | ✓ | **✓** |
| Sibling isolation | ✗ | ✗ | ✗ | ✓ | **✓** |
| No setup required | ✓ | ✓ | ✗ | ✗ | **✓** |
| No root privileges | ✓ | ✓ | ✗† | ✓* | **✓** |
| No PID 1 complexity | ✓ | ✓ | ✓ | ✗ | **✓** |

</div>

<div class="mt-2 text-sm">

- **Process groups/sessions**: processes can escape via `setsid()` or `setpgid()`
- **Cgroups**: reliable termination, but require setup and typically root
- **PID namespaces**: good isolation, but impose PID 1 init overhead

</div>

<div class="mt-1 text-xs opacity-70">
†Cgroup v2 delegation can avoid root but requires prior config. *User namespaces can avoid root.
Composing these in <strong>userspace</strong> creates race windows and fragile error handling.
</div>

<!--
Now let's look at process management mechanisms.

Process groups and sessions allow group termination, but processes can escape via setsid() or setpgid(). Manual PID tracking via /proc has race conditions.

Cgroups provide reliable termination, but require setup and typically need root. They also don't prevent signaling between siblings.

PID namespaces provide complete isolation, but impose PID 1 init overhead.

The fundamental problem is: composing these mechanisms in userspace creates race windows between steps. A process can fork between cgroup creation and migration. Error handling for partial failures is fragile.

Our proposed branch() syscall satisfies all requirements in a single atomic call.
-->

---

# Our Solution: The Branch Context

<div class="grid grid-cols-2 gap-5 text-base mt-2">

<div>

### Definition

A **branch context** encapsulates:

1. A **filesystem view**: origin's files overlaid with a copy-on-write delta layer (Δᵢ)
2. A **process group** whose side effects are confined to the context

### Lifecycle

```
  Fork ──► Explore ──► Commit (winner)
                   └──► Abort  (losers)
```

Created in **sets of N siblings** from a single frozen origin.

</div>

<div>

### Four Core Semantic Properties

<div class="border-l-4 border-blue-500 pl-3 mb-2">

**1. Frozen origin** — parent becomes read-only; no merge conflicts by construction

</div>

<div class="border-l-4 border-green-500 pl-3 mb-2">

**2. Parallel isolated execution** — N contexts run simultaneously, fully isolated

</div>

<div class="border-l-4 border-orange-500 pl-3 mb-2">

**3. First-commit-wins** — first to commit wins; siblings auto-invalidated

</div>

<div class="border-l-4 border-purple-500 pl-3">

**4. Nestable contexts** — fork sub-contexts to form an exploration tree

</div>

</div>

</div>

<!--
Now let me introduce our solution: the branch context.

A branch context encapsulates two things: a filesystem view — the origin's files overlaid with a copy-on-write delta layer — and a process group whose side effects are confined to the context.

The lifecycle is simple: Fork creates N sibling contexts from a frozen origin. Each context Explores independently. Then one Commits its changes atomically to the parent, and the losers are Aborted.

Four properties define the semantics.

First, frozen origin: the parent becomes read-only when branches exist, eliminating merge conflicts by construction.

Second, parallel isolated execution: all N contexts run simultaneously, fully isolated from each other.

Third, first-commit-wins: the first context to commit wins, and all siblings are automatically invalidated.

Fourth, nestability: a branch context can itself fork sub-contexts, forming an exploration tree — enabling patterns like Tree-of-Thoughts.
-->

---

# Architecture Overview

## Two-component design: BranchFS + branch() syscall

<div class="flex justify-center">
  <img src="/fig-architecture.png" class="rounded shadow-lg" style="max-height: 380px;" alt="Architecture overview" />
</div>

<div class="text-xs text-center mt-1 opacity-80">
<strong>branch()</strong> coordinates process creation and mount namespace isolation; <strong>BranchFS</strong> provides filesystem branching with copy-on-write semantics.
</div>

<!--
Here's the architecture. The parent process calls branch() to create three children. Each child runs in its own mount namespace.

BranchFS, a FUSE daemon, provides each child with an isolated copy-on-write view of the workspace. Each branch has its own delta layer that captures modifications via copy-on-write. Unmodified files are served directly from the base directory.

When a branch commits, its delta is applied atomically to the parent. Siblings are invalidated.

This is a two-component design: BranchFS handles filesystem isolation, and the branch() syscall handles process coordination. This separation of concerns is important — it means the syscall is filesystem-agnostic.
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

# Evaluation: Branch Creation — O(1)

<div class="grid grid-cols-2 gap-6 text-base mt-2">

<div>

### Creation Latency vs. Base Size

| Base Size (files) | Latency |
|:-:|:-:|
| 100 | 292 μs |
| 1,000 | 317 μs |
| 10,000 | 310 μs |

<div class="mt-3 p-2 bg-blue-50 rounded border border-blue-300 text-sm">
Creation latency remains <strong>under 350 μs</strong>, confirming <strong>O(1) cost</strong> — independent of base filesystem size.
</div>

Branch creation only allocates a new delta directory.

</div>

<div>

### Commit & Abort vs. Modification Size

| Mod. Size | Commit | Abort |
|:-:|:-:|:-:|
| 1 KB | 317 μs | 315 μs |
| 100 KB | 514 μs | 365 μs |
| 1 MB | 2.1 ms | 890 μs |

<div class="mt-3 p-2 bg-green-50 rounded border border-green-300 text-sm">
Commit cost is proportional to <strong>modified data volume</strong>, not total FS size. Under <strong>1 ms</strong> for small changes.
</div>

Abort is even faster — just removes the delta layer.

</div>

</div>

<div class="mt-2 text-xs text-center opacity-70">
Hardware: AMD Ryzen 5 5500U, 8 GB DDR4, NVMe SSD. Median of 10 trials. BranchFS: ~3,400 lines of Rust, FUSE 3.
</div>

<!--
Let me walk through the evaluation results. We tested BranchFS on a modest AMD Ryzen system with an NVMe SSD.

For branch creation, the key result is O(1) cost. Whether the base directory has 100, 1,000, or 10,000 files, creation latency remains under 350 microseconds. This is because creation just allocates a new delta directory — it doesn't copy any files.

For commit and abort, the cost is proportional to the modification size, not the total filesystem size. Commit takes about 317 microseconds for 1 kilobyte of changes, 514 microseconds for 100 kilobytes, and about 2 milliseconds for 1 megabyte.

Abort is even faster since it just removes the delta layer without copying anything.

These numbers are well within the requirements for agent workloads.
-->

---

# Evaluation: I/O Throughput

<div class="text-base mt-2">

### Sequential Read/Write (50 MB file, 64 KB blocks)

| Mode | Read | Write |
|:-:|:-:|:-:|
| Native filesystem | 8,800 MB/s | 576 MB/s |
| BranchFS (regular FUSE) | 1,655 MB/s | 631 MB/s |
| BranchFS (passthrough) | **7,236 MB/s** (82% native) | 719 MB/s |

</div>

<div class="grid grid-cols-2 gap-6 text-sm mt-3">

<div class="border-l-4 border-blue-500 pl-3">

**Passthrough mode** (`FOPEN_PASSTHROUGH`): bypasses the FUSE daemon for unmodified files → 82% of native read throughput

</div>

<div class="border-l-4 border-green-500 pl-3">

**Write exceeds native**: BranchFS treats `fsync` as no-op for ephemeral branches — durability enforced at commit time, not per-write

</div>

</div>

<div class="mt-4 p-3 bg-yellow-50 rounded border border-yellow-300 text-base text-center">
Agent workloads are dominated by <strong>LLM API latency</strong> (100 ms – 10 s).<br/>I/O overhead from BranchFS is <strong>negligible</strong> in practice.
</div>

<!--
For I/O throughput, we benchmarked sequential reads and writes on a 50 megabyte file.

With regular FUSE, read throughput is about 1.7 gigabytes per second — 19% of native. This gap reflects the FUSE kernel-to-userspace roundtrip.

But with FUSE passthrough mode, which bypasses the daemon for unmodified files, we achieve 7.2 gigabytes per second — 82% of native performance. This is a significant improvement.

Interestingly, write performance slightly exceeds native. This is because BranchFS treats fsync as a no-op for ephemeral branches — durability is enforced at commit time, not per-write. This avoids the SSD sync cost.

But the most important point is: agent workloads are dominated by LLM API latency, which is 100 milliseconds to 10 seconds. The I/O overhead from BranchFS is negligible in practice.
-->

---

# Related Work

<div class="grid grid-cols-2 gap-5 text-sm mt-1">

<div>

### Isolation Mechanisms

- **Containers** (Docker): namespaces + cgroups — general containment, not branching
- **gVisor**: kernel-level sandboxing
- **lwCs**: lightweight intra-process contexts
- **Dune**: HW virtualization for user-level sandboxing
- **Capsicum**: capability-based sandboxing

### OS Speculation & Transactions

- **Speculator**: speculative execution for distributed FS
- **TxOS**: OS-level ACID transactions

</div>

<div>

### Key Differentiators

<div class="border-l-4 border-blue-500 pl-3 mb-2">

**vs. Speculator**: parallel N-way exploration (not sequential); competitive first-commit-wins (not owner-decided); local agent workloads (not distributed systems)

</div>

<div class="border-l-4 border-green-500 pl-3 mb-2">

**vs. TxOS**: nestable (not flat); long-lived (not short); userspace FUSE (not deep kernel mods)

</div>

<div class="border-l-4 border-orange-500 pl-3">

**vs. Containers/VMs**: lightweight CoW branching with commit/abort semantics — not general containment

</div>

</div>

</div>

<!--
Let me briefly position this against related work.

For isolation, containers and VMs provide general-purpose containment but not branching semantics with commit and abort.

The closest prior work is Speculator, which pioneered OS-level speculative execution. But Speculator targets distributed file systems for network latency reduction, uses sequential speculation with owner-decided acceptance. Our branch contexts support parallel N-way exploration with competitive first-commit-wins resolution for local agent workloads.

TxOS introduced OS-level ACID transactions, but they're flat, short-lived, and require deep kernel modifications. Our branch contexts are nestable, can persist for arbitrary durations, and operate entirely in userspace via FUSE.
-->

---

# Discussion and Future Work

<div class="grid grid-cols-2 gap-5 text-sm mt-1">

<div>

### Current Limitations

- **External side effects** (network, IPC) not rolled back on abort
- **Single-winner** semantics only — no multi-branch merge
- File-level CoW limitations: symlinks, hardlinks, special files
- **BR_MEMORY** not yet implemented

### Broader Applicability

- `n_branches=1` → **try-and-rollback**
  - Package management: try upgrade, abort if it breaks
  - System configuration tuning

</div>

<div>

### Future Directions

<div class="border-l-4 border-blue-500 pl-3 mb-2">

**Effect gating** — buffer network/IPC until commit; agent gateways (e.g., Agentry) provide natural interposition points

</div>

<div class="border-l-4 border-green-500 pl-3 mb-2">

**Multi-branch merge** — union of non-overlapping changes; conflict detection for overlapping files

</div>

<div class="border-l-4 border-purple-500 pl-3 mb-2">

**Implementation roadmap** — prototype `branch()` on Linux 6.19; BR_FS + BR_ISOLATE first

</div>

<div class="border-l-4 border-orange-500 pl-3">

**Agent integration** — most agents checkpoint via files → BR_FS-only covers most current use cases

</div>

</div>

</div>

<!--
Let me discuss limitations and future directions.

Currently, we only isolate filesystem state. External side effects like network calls and IPC are not rolled back on abort. To address this, we're exploring effect gating — buffering external actions until commit and discarding them on abort. Agent gateways like Agentry provide natural interposition points for this.

We also support only single-winner semantics. Multi-branch merge — combining non-overlapping changes from multiple branches — is an important future direction.

The implementation roadmap: we plan to prototype the branch() syscall on Linux 6.19, starting with BR_FS and BR_ISOLATE flags, with memory branching as a follow-on effort.

An important practical observation: most current AI agents — Claude Code, SWE-Agent, OpenHands — checkpoint primarily via files rather than process memory. This means BR_FS-only mode covers most use cases today. Agents can run unmodified inside their branch's mount namespace.

Beyond AI agents, the fork-explore-commit abstraction generalizes. With n_branches equals 1, it provides try-and-rollback semantics useful for package management and system configuration tuning.
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
