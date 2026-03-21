---
title: "Fork, Explore, Commit: OS Primitives for Agentic Exploration"
author: Cong Wang, Yusheng Zheng
event: Agentic OS Workshop, ASPLOS 2026
---

# Fork, Explore, Commit
## OS Primitives for Agentic Exploration

**Cong Wang** (Multikernel Technologies) &  **Yusheng Zheng** (UC Santa Cruz)

Agentic OS Workshop, ASPLOS 2026 | Pittsburgh, USA

<!-- ~1 min -->

---

# The Problem: Agents Have Side Effects

- Modern AI agents execute **multi-step tasks**: shell commands, file edits, package installs
- Increasingly, agents explore **multiple solution paths in parallel**
  - Tree-of-Thoughts, Reflexion, SWE-agent, OpenHands
- Each path produces **irreversible side effects** on the filesystem and spawns processes
- Need: **isolate** each path, **commit** the winner, **discard** the rest

> Example: A software engineering agent tries 3 candidate fixes simultaneously, commits only the one that passes tests.

<!-- ~2 min -->

---

# What Do Agents Use Today?

| Approach | Limitation |
|----------|-----------|
| Git stashing | Cannot capture non-tracked files (build artifacts, node_modules) |
| Temporary directories | Manual setup/teardown, no atomic commit |
| Container clones | Heavyweight, requires root, slow startup |
| Per-file snapshots | Miss shell command side effects, no parallel branching |

**None** captures all filesystem modifications with atomic commit/rollback.

<!-- ~1 min -->

---

# Six Requirements for Agentic Exploration

| # | Requirement | Why |
|---|------------|-----|
| R1 | **Isolated parallel execution** | Concurrent paths modify same files |
| R2 | **Atomic commit + single-winner** | Apply winner's changes, invalidate siblings |
| R3 | **Hierarchical nesting** | Tree-of-Thoughts explores sub-variants |
| R4 | **Complete filesystem coverage** | Must capture *all* modifications, not just tracked files |
| R5 | **Lightweight, unprivileged, portable** | Sub-ms creation, no root, any FS |
| R6 | **Process coordination** | Reliable termination, sibling isolation |

No existing OS mechanism satisfies all six.

<!-- ~2 min -->

---

# Our Solution: The Branch Context

A new OS abstraction: an isolated execution environment with:

1. **Frozen origin**: parent becomes read-only, no merge conflicts
2. **Parallel isolated execution**: N contexts run simultaneously, fully isolated
3. **First-commit-wins**: first to commit wins, siblings invalidated
4. **Nesting**: branch contexts can fork sub-contexts, forming a tree

## Lifecycle

```
  Fork ──► Explore ──► Commit (winner)
                   └──► Abort  (losers)
```

<!-- ~1.5 min -->

---

# Architecture Overview

```
       ┌─────────────────┐
       │  Parent Process  │
       └────────┬────────┘
                │ branch(N=3)
       ┌────────┼────────┐
       ▼        ▼        ▼
   ┌───────┐┌───────┐┌───────┐
   │Child 1││Child 2││Child 3│   ← Process isolation
   └───┬───┘└───┬───┘└───┬───┘
       │        │        │
   ┌───▼───┐┌──▼────┐┌──▼────┐
   │Mount  ││Mount  ││Mount  │   ← Mount namespace
   │  NS   ││  NS   ││  NS   │
   └───┬───┘└───┬───┘└───┬───┘
       │        │        │
   ════╪════════╪════════╪════
       │   BranchFS (FUSE)│      ← Filesystem branching
   ════╪════════╪════════╪════
       ▼        ▼        ▼
   ┌──────┐┌──────┐┌──────┐
   │ Δ₁   ││ Δ₂   ││ Δ₃   │     ← Per-branch deltas
   └──┬───┘└──┬───┘└──┬───┘
      └───────┼───────┘
              ▼
   ┌─────────────────────┐
   │   Base Directory     │       ← Original files
   └─────────────────────┘
```

Two components: **BranchFS** (filesystem) + **branch()** (process coordination)

<!-- ~1.5 min -->

---

# BranchFS: FUSE-Based Copy-on-Write

## File-Level CoW
- First write → copy entire file to branch's **delta layer** (per-branch directory)
- Subsequent reads/writes served from delta copy
- Unmodified files resolved by walking the **branch chain** to base

## Branch Chain Resolution
1. Check current branch's delta
2. Walk ancestor branches
3. Fall back to base directory
4. **Tombstone markers** prevent deleted files from reappearing

## Key Properties
- **O(1) branch creation**: just create a delta directory
- **No root privileges**: runs entirely as FUSE daemon
- **Portable**: works on ext4, XFS, NFS, etc.

<!-- ~2 min -->

---

# BranchFS: Commit & Abort

## Commit (atomic, to parent)
1. Collect modified files + tombstones from delta
2. Apply deletions first
3. Copy modified files to parent's delta
4. Increment parent's **epoch counter** → invalidates all siblings
5. Invalidated branches get SIGBUS on mmap access

## Abort
- Discard delta layer: **near-zero cost**
- Siblings remain valid

## Isolation
- Each branch accessible via **@branch paths**: `/mnt/workspace/@feature-a/`
- Per-agent mount namespace for multi-agent workflows

<!-- ~1 min -->

---

# BranchContext: Agent Integration Library

Python library wrapping BranchFS into high-level exploration patterns:

| Pattern | Strategy |
|---------|----------|
| **Speculate** | Race N candidates, first success wins |
| **BestOfN** | Run N candidates, commit highest-scoring |
| **Reflexion** | Sequential retry with failure feedback |
| **TreeOfThoughts** | Hierarchical exploration with nested branches |
| **BeamSearch** | Keep top-K branches alive at each depth |
| **Tournament** | Pairwise elimination via judge function |
| **Cascaded** | Start with 1, adaptively fan out on failure |

Each pattern manages **branch lifecycle** + **process isolation** internally.
Agent code only supplies per-branch task logic.

**github.com/multikernel/branching**

<!-- ~2 min -->

---

# The branch() Syscall (Proposed)

Why a syscall? **Atomic composition** of cgroups + PID ns + mount ns + FS branches.

```c
long branch(int op, union branch_attr *attr, size_t size);
```

## Three Operations
- **BR_CREATE**: fork N children, each in its own branch
- **BR_COMMIT**: apply FS changes, terminate siblings
- **BR_ABORT**: discard changes, terminate branch

## Composable Flags
| Flag | Effect |
|------|--------|
| BR_FS | Mount namespace + branch (required) |
| BR_MEMORY | Page-table CoW for memory branching |
| BR_ISOLATE | Signal/ptrace barriers between siblings |
| BR_CLOSE_FDS | Close inherited FDs |

Integrates with **any** branching FS via generic ioctls (FS_IOC_BRANCH_*)

<!-- ~1.5 min -->

---

# Preliminary Evaluation

## Branch Creation — O(1), independent of base size

| Base Size (files) | Creation Latency |
|:-:|:-:|
| 100 | 292 μs |
| 1,000 | 317 μs |
| 10,000 | 310 μs |

## Commit & Abort — proportional to modification size

| Mod. Size | Commit | Abort |
|:-:|:-:|:-:|
| 1 KB | 317 μs | 315 μs |
| 100 KB | 514 μs | 365 μs |
| 1 MB | 2,100 μs | 890 μs |

## I/O Throughput

| Mode | Read | Write |
|:-:|:-:|:-:|
| Native | 8,800 MB/s | 576 MB/s |
| BranchFS (FUSE) | 1,655 MB/s | 631 MB/s |
| BranchFS (passthrough) | **7,236 MB/s** (82% native) | 719 MB/s |

Sufficient for agent workloads dominated by LLM API latency (100ms to 10s).

<!-- ~1.5 min -->

---

# Discussion & Future Work

## Semantic Extensions
- Current: filesystem isolation only
- Future: **effect gating** to buffer network/IPC until commit, discard on abort
- Future: **multi-branch merge** to combine results from multiple branches

## Implementation Roadmap
- **branch() syscall**: prototype on Linux 6.19 (BR_FS + BR_ISOLATE first)
- **BR_MEMORY**: follow-on effort (page table complexity)
- Most current agents checkpoint via files → BR_FS-only covers most use cases

## Beyond AI Agents
- `n_branches=1` → **try-and-rollback** for package management, system tuning

<!-- ~1 min -->

---

# Key Takeaways

1. **Agentic exploration** is a first-class OS concern: agents need isolated, parallel workspaces with atomic commit/rollback

2. **Branch context** = new OS abstraction with fork/explore/commit lifecycle and first-commit-wins resolution

3. **BranchFS** = working FUSE implementation: O(1) creation, no root, portable across filesystems

4. **BranchContext** = Python library with 7 ready-to-use agent exploration patterns

5. **branch() syscall** = proposed kernel primitive for atomic process + FS coordination

### Try it now

- BranchFS: **github.com/multikernel/branchfs**
- BranchContext: **github.com/multikernel/branching**

<!-- ~0.5 min -->

---

# Thank You — Questions?

**Cong Wang** — cwang@multikernel.io

**Yusheng Zheng** — yzhen165@ucsc.edu

Code: github.com/multikernel/branchfs | github.com/multikernel/branching
