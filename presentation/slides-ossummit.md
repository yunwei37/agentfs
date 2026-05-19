---
theme: academic
title: 'Fork, Explore, Commit: Linux Primitives for AI Agents Exploration'
info: |
  ## Fork, Explore, Commit: Linux Primitives for AI Agents Exploration
  Open Source Summit 2026
class: text-center
coverDate: Open Source Summit 2026
drawings:
  persist: false
transition: fade
mdc: true
layout: cover
colorSchema: light
---

<div class="text-center">

<div class="text-4xl leading-relaxed">Fork, Explore, Commit:<br/>Linux Primitives for AI Agents Exploration</div>

<div class="mt-8 text-xl">
Cong Wang¹, Yusheng Zheng²
</div>

<div class="text-sm opacity-80 mt-2">
¹Multikernel Technologies, Inc. · ²eunomia-bpf / UCSC
</div>

</div>

<!--
Good morning everyone. I'm here to talk about a new pair of Linux primitives we have been building (a userspace filesystem called BranchFS, and a proposed kernel syscall called branch()) that together give AI agents something the kernel does not currently provide: a clean fork-explore-commit lifecycle for filesystem and process state.

This is joint work between Multikernel Technologies and eunomia-bpf. The userspace piece is open source and works on any Linux today. The kernel piece is a working prototype against vanilla Linux 6.17.

Over the next thirty minutes I'll do four things. First, ground us in what an AI agent actually looks like at the Linux level, because if you have not been chasing this hype cycle, you may be surprised how mundane it is from the kernel's point of view. Second, walk through why nothing already in Linux quite fits: OverlayFS, Btrfs, namespaces, cgroups all get partway. Third, show you BranchFS, the FUSE filesystem we wrote to fill that gap. And fourth, walk through the kernel patch series for branch(), boot it under QEMU, and talk about what we want to upstream.

I'll leave roughly ten minutes for questions at the end.
-->

---

# Agenda

<div class="text-xl mt-5 leading-loose">

1. **Background**: AI agents needs support for fork and exploration
2. **Problem**: execution leaves real filesystem and process side effects
3. **Requirements**: what fork-explore-commit needs from the OS
4. **Design**: branch contexts as the abstraction
5. **Implementation**: BranchFS in userspace, `branch()` in the kernel
6. **Status**: latency, demo, limitations, roadmap

</div>

<!--
Here is the roadmap for the talk.

First, I'll give the background: what an agent looks like to Linux. It is a normal process doing normal filesystem operations.

Then I'll state the problem: those normal operations become speculative side effects when agents explore multiple paths.

From there, we'll turn that problem into requirements, then into the branch context design. The implementation has two halves: BranchFS in userspace and branch() in the kernel. Finally, I'll show latency numbers, a demo, current limitations, and the roadmap.
-->

---

# What Is an AI Agent?

<div class="grid grid-cols-2 gap-6 text-base mt-3">

<div class="text-xl leading-relaxed">

An **AI agent** is a control loop:

1. **Reason** with an LLM
2. **Act** in a local workspace with tool calls (shell commands, file edits)
3. **Observe** the result and repeat

</div>

<div>

### Examples

| Tool class | What it does locally |
|------------|----------------------|
| **Claude Code, SWE-agent, OpenHands** | edit your repo and run your tests |
| **Aider, Cursor agents** | same loop inside an editor |
| **Devin, OpenAI Codex CLI** | same loop on a hosted machine |

<!-- <div class="mt-4 p-3 rounded border-2 border-dashed border-red-400 text-sm">
In systems terms: the agent has authority to mutate a workspace.
</div> -->

</div>

</div>

<!--
Let me start by demystifying what an AI agent actually is, because the term is doing a lot of work.

For this talk, an agent is a control loop. It reasons with a language model, acts in a local workspace, observes the result, and repeats. The action is usually either a shell command or a file edit.

The examples you've probably heard of all fit this shape. Claude Code, SWE-agent, and OpenHands edit your repository and run your tests. Aider and Cursor agents do the same thing inside an editor. Devin and the Codex CLI do it on hosted machines.

Next, let's look at the pattern that makes this interesting: agents are starting to fork exploration paths.
-->

---

# Agent Exploration and Forking

<div class="text-lg mt-4 leading-relaxed">

Agents increasingly try **multiple paths** to solve a problem:

| Pattern | What it does |
|---------|---------------------|
| **Parallel Work** | Try several solutions at once, keep the best result |
| **Tree-of-Thoughts** | Explore a tree of reasoning paths, prune weak ones |
| **RL rollout** | Run trials and score the outcome as reward |
| **Speculate** | Start likely paths early |

<div class="mt-5 p-3 rounded border-2 border-dashed border-red-400 text-lg">
<strong>Example:</strong> agent tries 3 candidate bugfixes on the same repo; commit only the one whose tests pass.
</div>

</div>

<!--
Here's the pattern that motivates everything else in this talk.

Agents are starting to do something more interesting than just running serially. They try multiple paths in parallel and keep the one that worked. This is well-studied in the LLM research literature: Best-of-N, Tree-of-Thoughts, RL rollouts, speculative execution. For RL rollout, think of a complete trial run whose outcome is scored by a reward signal. The names don't really matter. What matters is the shape: fan out into N attempts, let them run independently, commit one, discard the rest.

Now, if you're going to run three candidate bugfixes against the same repository in parallel, you have a problem: they all want to modify the same files. You need isolation.
-->

---

# Agents on Linux: Processes with Effects

<div class="grid grid-cols-2 gap-6 text-lg mt-3">

<div>

From `strace`, an agent looks like a developer typing fast:

```text
execve("/bin/sh", ["sh", "-c", "pytest -x"], ...)
openat(AT_FDCWD, "src/parser.py", O_WRONLY|O_TRUNC)
write(3, "def parse(s):\n    ...", 4096)
execve("/usr/bin/git", ["git", "apply", "fix.patch"])
execve("/usr/bin/npm", ["npm", "install", "lodash"])
unlink("node_modules/.package-lock.json")
```

</div>

<div>

### What this means for Linux

- They run as **ordinary processes**
- They produce **unpredictable side effects**: dirty trees, installed packages, build artifacts, modified dotfiles

<div class="mt-4 p-4 bg-blue-50 rounded border border-blue-300 text-lg leading-relaxed">
The OS sees a normal Unix workload. The agent's <em>intent</em> ("this is one of three things I'm trying") is invisible.
</div>

</div>

</div>

<!--
Now let's look at that same pattern from Linux's point of view.

If you strace one of these tools, you see the same calls you'd see from a developer at a terminal. execve of /bin/sh. openat and write on source files. git apply. npm install. unlink. The agent is just typing faster than you can.

So the process is ordinary, but the side effects are real. It generates dirty working trees. It installs packages. It modifies dotfiles. It leaves build artifacts. There is nothing in Linux today that knows these operations are speculative, that this is one of several paths being tried.

That's the gap we're trying to fill.
-->

---

# Runtime Requirements for Agentic Exploration

<div class="text-base mt-2">

| # | Requirement | Why |
|---|------------|-----|
| **R1** | **Isolated parallel execution** | Concurrent paths modify same files |
| **R2** | **Hierarchical nesting** | Tree-of-Thoughts explores sub-variants |
| **R3** | **Complete filesystem coverage** | Capture *all* modifications, not just tracked files |
| **R4** | **Lightweight, unprivileged, portable** | Sub-ms creation, no root, any FS (ext4, XFS, NFS...) |
| **R5** | **Coordination** | For multi-agent, reliable termination, sibling isolation |

</div>

<div class="mt-3 p-3 bg-yellow-50 rounded border border-yellow-300 text-base text-center">
No existing Linux mechanism satisfies these requirements. Let's walk through why.
</div>

<!--
Before we go look at the existing mechanisms, let me consolidate what we actually need into five requirements. I'll wave at these whenever I'm explaining why something falls short.

R1: isolated parallel execution. The siblings run at the same time and they may touch the same files. So they need separate views.

R2: hierarchical nesting. Tree-of-Thoughts and similar patterns recurse. A branch may itself spawn sub-branches. We need a tree, not a flat fan-out.

R3: complete filesystem coverage. This is the killer for git stash. Agents do things like npm install, pip install, cargo build. The interesting filesystem changes are in directories that .gitignore lists. We have to capture all of it.

R4: lightweight, unprivileged, portable. Branch creation should be in the microsecond range so agents can branch per reasoning step. No root, because agents run in CI, in containers, on developer laptops. Portable across filesystems, because not everyone is on btrfs.

R5: process coordination. Each branch spawns its own processes, a test runner, a compiler, an installer. When we commit or abort, all of those processes must die reliably, and one branch's processes must not be able to signal another branch's processes.

That's the rubric. Now let's go grade current sandboxes against it.
-->

---

# Current Approaches Fall Short

<div class="grid grid-cols-2 gap-5 text-base mt-2">

<div>

| Hack | Why it hurts |
|------|--------------|
| `cp -r workspace branch1/` | 10 GB monorepo -> slow, disk hog |
| `git stash` per attempt | Misses `node_modules`, build artifacts |
| Docker container per try | Heavyweight, needs daemon, root |
| One workspace, retry serially | No parallelism, wall-clock loss |
| `chroot` + bind mounts | Privileged, racy setup, no atomic commit |

</div>

<div class="text-xl leading-relaxed">

### What actually needs:

- **One namespace** the agent lives in
- **N isolated branches** of it
- **Nested branches** for recursive exploration
- **No root**, **no daemon**, **portable**

<div class="mt-5 p-4 bg-yellow-50 rounded border border-yellow-300 text-xl leading-relaxed">
Let's see what currently we can build on, and why it doesn't meet the requirements.
</div>

</div>

</div>

<!--
What do people do today? They cp -r the whole workspace, which is fine if your repo is small and miserable if it's a ten gigabyte monorepo. They git stash between attempts, which only captures tracked files, not the node_modules directory, not your build output. They spin up a Docker container per attempt, which requires the docker daemon, often root, and adds startup latency that's huge relative to the actual exploration cost. Or they just give up on parallelism and retry serially, which throws away the whole point.

There is also a more sophisticated camp that uses chroot plus bind mounts plus their own home-grown cleanup. That's the closest in spirit to what we want, but it's racy to set up and it still doesn't give you an atomic commit.

What we actually want is on the right: one workspace path the agent lives in, N copy-on-write branches of it, first commit wins, losing branches are auto-discarded, and crucially: no root, no daemon, portable across whatever filesystem you happen to be on. That's the design target for the rest of the talk.
-->

---

# Filesystem Branching in Linux Today

<div class="grid grid-cols-2 gap-4 text-xs mt-1">

<div>

### OverlayFS

```bash
sudo mount -t overlay overlay \
  -o lowerdir=/repo,upperdir=…,workdir=… \
  /mnt/branch1
```

✓ Per-branch view · ✓ All modifications captured · ✓ Portable

✗ **`sudo mount`** required (rootless overlay is fragile)
✗ **Cannot commit changes back**: `rsync upperdir/ → lowerdir/` skips whiteout deletions
✗ **No automatic cleanup for losing branches**
✗ Nesting is complex and easy to break

</div>

<div>

### Btrfs / ZFS subvolumes

```bash
$ btrfs subvolume snapshot /repo /repo-b1
$ btrfs subvolume snapshot /repo /repo-b2
```

✓ O(1) creation · ✓ Block-level CoW · ✓ Nested subvolumes first-class

✗ **FS-locked**: your CI runs ext4, your colleague's laptop runs ext4
✗ **Cannot commit changes back**: `btrfs subvolume promote` doesn't exist
✗ **NFS / tmpfs / overlayfs** unsupported
✗ ZFS: `zfs promote` inverts parent ↔ child (wrong shape)

</div>

</div>

<div class="mt-4 p-3 bg-red-50 rounded border border-red-300 text-sm text-center">
Both come close. Both miss <strong>committing changes back</strong>, <strong>discarding losing branches</strong>, and either <strong>unprivileged</strong> or <strong>portable</strong> operation.
</div>

<!--
Let's look at the two filesystem branching options people reach for first.

Left side: OverlayFS. Mount with a lowerdir, an upperdir, and a workdir. You get a unioned view where reads fall through and writes land in the upper. Make N of these and you have N isolated branches.

Three checks pass: per-branch view, all modifications captured, portable over any lower filesystem.

Four checks fail. The mount command needs root, rootless overlay exists since kernel 5.11 but is fragile across distros. There's no clean way to commit changes back; if you try to rsync the upperdir back to the lower, you miss the character-device whiteouts that represent deletions, and the deleted file reappears on next mount. There's no automatic cleanup for losing branches. Nesting two overlays is technically supported but complex and easy to break.

Right side: Btrfs and ZFS subvolumes. btrfs subvolume snapshot is the right shape conceptually. Truly O(1), block-level CoW, nested subvolumes are first-class. Three checks pass.

But four checks fail. It's filesystem-locked: your CI is on ext4, your colleague is on ext4, your customers are on whatever they're on. There's no clean way to commit changes back; btrfs subvolume promote doesn't exist, so you're back to rsync. NFS, tmpfs, overlayfs all unsupported because they're not Btrfs. ZFS has zfs promote, but it inverts the parent-child relationship in the dataset tree, which is not the operation we want, plus the licensing issue keeps it out of mainline.

The red box is the takeaway: both come close. The shape is roughly right. But both miss committing changes back, both miss discarding losing branches, and they fail R5 in opposite ways, OverlayFS fails on unprivileged, Btrfs fails on portable.
-->

---

# Process Isolation in Linux Today

<div class="text-sm mt-3">

Each primitive does one thing well, none does the whole job:

| Primitive | What it gives us | Cost |
|-----------|------------------|------|
| **PID namespace** | Reliable kill of all descendants | PID 1 init overhead |
| **Mount namespace** | Private view of `/mnt/workspace` | Needed anyway for FS isolation |
| **cgroup v2** | Reliable group termination via `cgroup.kill` (5.14+) | Setup, often needs root |
| **`clone3()`** | Compose namespaces at process creation | Multi-step, no atomicity |
| `setpgid` / `setsid` | Process groups | Escapable, child can `setsid()` and leave |

</div>

<div class="mt-4 p-3 bg-yellow-50 rounded border border-yellow-300 text-base text-center">
The kernel exposes the right <strong>ingredients</strong>, but not one operation that <strong>combines them safely</strong>.
</div>

<!--
Mechanism three: the process side. PID namespaces, mount namespaces, cgroup v2, clone3, the whole namespace toolkit.

Each piece exists and each piece individually does something useful. PID namespaces give you a reliable kill of everything inside. cgroup v2 added cgroup.kill in 5.14 which is a much cleaner group-kill primitive than walking PIDs. clone3 lets you compose namespaces atomically at process creation. Mount namespaces give us the private workspace view we need anyway. Process groups via setpgid and setsid are the oldest mechanism, they work for cooperative children, but a child can call setsid() and leave the group, so they can't be used for isolation against hostile or buggy code.

The thing I want you to leave this slide with is the line at the bottom: the kernel exposes the right ingredients. None of them is missing. What's missing is one operation that combines them safely, the single operation that says "give me all of these together, atomically." That's what the next slide is going to show you.
-->

---

# Composition is not straightforward

<div class="grid grid-cols-2 gap-6 text-base mt-2">

<div>

### To make one branch in userspace

```text
1. Mount the FS branch
2. unshare(CLONE_NEWNS)
3. clone3(NEWPID|NEWNS)
4. Move PID into cgroup
5. Install signal/ptrace fence
   (no primitive, DIY)
```

Each step can fail. Each gap between steps is a **race window**; making cleanup reliable adds overhead.

</div>

<div>

### The bug between steps 3 and 4

```text
parent: clone3() returns PID 12345
parent: ... about to add 12345 to cgroup
12345:  fork() → child 12346
parent: cgroup_add(12345), but 12346 is loose
parent: later, cgroup.kill, 12346 survives
parent: /proc-walk to find it. Maybe.
```

<div class="mt-4 p-3 bg-red-50 rounded border border-red-300 text-base">
The forked grandchild escapes the cgroup. Reliable cleanup now needs a PID-1 babysitter or a single atomic primitive.
</div>

</div>

</div>

<!--
Here is the race, concretely.

On the left: the steps you have to do to create one branch. Mount the FS branch. unshare the mount namespace. clone3 with the right flags. Move the resulting PID into a cgroup. And install some kind of signal and ptrace fence between siblings, for which there is no kernel primitive at all.

Between any two of those steps, the child can be doing things. The window between step 3 and step 4 is the textbook one, shown on the right. clone3 returns. The parent has the child's PID, 12345. The parent is about to add 12345 to its cgroup. Before that line executes, the child (which is now running) calls fork. Now there's a 12346 grandchild that was never in the cgroup.

When you later try to kill this branch via cgroup.kill, 12345 dies. 12346 survives, orphaned. To find and kill it, you have to walk /proc looking for processes whose parent is now PID 1. That's not isolation, that's a hunt.

You can fix this by running a PID 1 inside the branch that babysits everything. But that's the exact overhead that ruled out PID namespaces in the first place.

The pattern keeps repeating: every userspace composition has a similar window. The fundamental issue is that the kernel doesn't have a single operation that says "do all of this together." Until it does, branch contexts have to live with these races, or fall back to heavyweight isolation.
-->


---

# The Atomic Composition Problem

<div class="text-sm mt-1">

<div class="grid grid-cols-2 gap-5">

<div>

### What we keep finding

Every existing OS primitive does **one thing well**: but agentic exploration needs **all of them, composed atomically**:

```text
   ┌── FS branch (per-branch upperdir)
   │
   ├── new mount namespace
   │
   ├── new PID namespace (or cgroup)
   │
   ├── signal/ptrace fence vs. siblings
   │
   └── child PID returned to parent
```

Each step in userspace = a **race window** + a **partial-failure path**.

</div>

<div>

### Precedent: why `clone()` exists

<div class="space-y-3 text-base leading-relaxed">

- Before `clone()`, threads were almost buildable from `fork()` + shared memory + signals.
- Linux added `clone()` because that composition needed to be **atomic**.
- Branch contexts make the same argument for **filesystem state + process isolation**.

</div>

<div class="mt-4 p-3 bg-blue-50 rounded border border-blue-300 text-base">
The missing piece is not another sandbox. It is an OS-level mechanism: <strong>branch()</strong>. The pieces exist, but Linux lacks one operation that combines them safely.
</div>

</div>

</div>

</div>

<!--
Let me consolidate the last three slides into one argument, because this is the pivot of the talk.

What we keep finding when we go down the existing-mechanisms list is that every individual primitive does one thing well. OverlayFS gives you per-branch views. Btrfs gives you O(1) snapshots. Cgroups give you reliable group termination. Mount namespaces give you isolated mount tables. clone3 gives you composable namespaces at fork.

The thing we need (agentic exploration) needs all of those pieces, but composed atomically. The agent says "give me three branches" and we need: a filesystem branch, a mount namespace, a process group with reliable termination, a fence between siblings, and a child PID back to the parent, all in one operation that either fully succeeds or fully cleans up.

This is not a new shape of problem in Linux. This is exactly why clone() exists. Before clone, you could almost build threads out of fork plus shared memory plus signal-based scheduling. People did, and it was awful, and there were race windows. Linus added clone() so the kernel could do the composition atomically. We are making the same argument: the pieces for fork-branch-fence-commit exist, but Linux lacks one kernel operation that combines them safely.

That's the pivot. From here on, I'm going to show you that operation. We've split it into two pieces, a filesystem called BranchFS that you can install today, and a kernel syscall called branch() that we have a working prototype of. Let's look at each.
-->

---

# Branch Contexts: Our Solution

<div class="grid grid-cols-2 gap-4 text-sm mt-1">

<div>

### Definition

A **branch context** = CoW filesystem view (Δᵢ) + confined process group

```text
Fork ──► Explore ──► Commit (winner)
                 └──► Abort  (losers)
```

### Four core properties

1. **Frozen origin**: parent read-only while branches exist
2. **Parallel isolated execution**: N siblings, fully isolated
3. **First-commit-wins**: siblings auto-invalidated
4. **Nestable**: a branch may fork sub-branches

</div>

<div>

### Architecture

<img src="/fig-architecture.png" class="rounded shadow" style="max-height: 280px;" alt="Architecture" />

<div class="text-xs opacity-80 mt-1">
<strong>branch()</strong> = process coordination (kernel); <strong>BranchFS</strong> = filesystem CoW (FUSE userspace)
</div>

</div>

</div>

<!--
Here is the abstraction itself. We call it a branch context. It encapsulates two things: a copy-on-write filesystem view, which we'll write as delta-sub-i, and a confined process group. Together, those two things form one branch.

The lifecycle has three phases. Fork: you create N siblings from a frozen origin. Explore: each sibling runs independently, accumulating filesystem and process state. Commit or Abort: a sibling that has decided it wants its changes applies them atomically to the parent and its siblings die; an aborting sibling just discards its delta with no effect on anyone else.

Four properties define the semantics, and these are the things that make branch contexts different from "a bunch of OverlayFS mounts behind some scripts."

Frozen origin means the parent's state is read-only while branches exist. There's nothing for the branches to merge against, because the parent isn't moving. This eliminates a whole class of conflict-resolution complexity.

Parallel isolated execution means all N siblings run simultaneously. They are completely walled off from each other. One sibling cannot observe or modify another's state, even though they share an ancestor.

First-commit-wins is our resolution rule. Any sibling can commit. The first one to commit wins atomically; all others are invalidated. This is the right choice for AI exploration because the orchestrator doesn't know which path will succeed.

Nestable means a branch may itself fork sub-branches. This forms a tree. Each level commits to its immediate parent. This matches Tree-of-Thoughts and similar patterns directly.

On the right is the architecture, the picture you'll see for the rest of the talk. Parent process at the top issues branch(N=3). It produces three child processes, each in its own mount namespace, each looking at its own BranchFS delta layer, all backed by the same base directory. The two halves we're going to implement are: branch() (that's the kernel syscall coordinating processes and namespaces) and BranchFS, that's the FUSE filesystem providing the deltas. Let's dig into BranchFS first.
-->

---

# BranchFS

<div class="grid grid-cols-2 gap-5 text-sm mt-3">

<div>

### What it is

- **~3,400 lines of Rust**, FUSE 3
- Userspace daemon: **no root**, no kernel module
- Portable: **ext4, XFS, btrfs, tmpfs, NFS**, any POSIX FS
- MIT / Apache-2.0
- `github.com/multikernel/branchfs`

</div>

<div>

### Using it

```bash
$ branchfs mount /repo /mnt/work
$ branchctl create /mnt/work feature-a
@feature-a
$ cd /mnt/work/@feature-a
$ vim src/parser.py && make test
$ branchctl commit /mnt/work/@feature-a
```

</div>

</div>

<!--
BranchFS at a glance. About 3,400 lines of Rust. It uses the fuser library, which is the standard Rust binding to the FUSE 3 protocol.

It runs entirely as a userspace daemon. No kernel module, no setup, no privileged install. That gets us three things FUSE traditionally gives you: anyone can run it because it's just cargo install; it's portable across kernels because there's no out-of-tree module to maintain against six distro kernels; and when we screw up, we panic a userspace daemon, not your kernel. The performance gap that FUSE used to have has largely closed with FUSE 3 passthrough mode, which we'll cover in a couple of slides.

It works over any filesystem you can point it at: ext4, XFS, btrfs, tmpfs, NFS, doesn't matter. Open source under MIT and Apache 2.0 dual license.

On the right, what using it actually looks like. Mount BranchFS over your repository at some mountpoint. Create a named branch with branchctl; you get back an @-prefixed path which is the virtual directory for that branch. cd into it and work as if you were in a normal repo. The branch sees every file in the underlying repo, but any writes you make are captured into a delta layer for that branch. When you're done, branchctl commit applies the delta to the underlying repo atomically; branchctl abort throws it away.

That's the user interface. Let's look at how it works underneath.
-->

---

# File-Level Copy-on-Write

<div class="text-sm mt-3">

- First write to a file → **copy the whole file** into the branch's delta directory
- Subsequent reads and writes to that file are served from the delta
- Unmodified files: pass-through to the base (or an ancestor branch that modified it)

</div>

<div class="grid grid-cols-2 gap-5 text-sm mt-4">

<div>

### Trade-off vs. block-level CoW

| | Block-level (Btrfs) | File-level (BranchFS) |
|---|---|---|
| First write of 1 MB file (1 byte changed) | ~4 KB | 1 MB |
| Implementation | Kernel-level COW | One FUSE handler |
| FS dependency | Btrfs-only | Anything |

</div>

<div>

### Why this works for agents

- Agent-touched files: source, config, small artifacts
- Typical size: KB to low MB
- Even huge `node_modules` only matters on the first write
- 1 MB copy ≈ 200 µs, still 1000× smaller than an LLM call

</div>

</div>

<!--
The core mechanism is file-level copy-on-write.

The first time a branch writes to a file, BranchFS copies the entire file from wherever it currently lives (the base, or an ancestor branch) into the branch's delta directory. From then on, all reads and writes for that file in that branch hit the delta copy. Unmodified files are passed through.

This is coarser than block-level CoW, which Btrfs uses. The trade-off table on the left tells the story honestly. If you touch one byte of a one-megabyte file, Btrfs copies four kilobytes. We copy the whole megabyte. That's a real cost.

But the trade-off pays for itself two ways. First, the implementation is dramatically simpler. We just call libc copy_file_range from a FUSE handler. There is no kernel work, no metadata bookkeeping. Second, we are not tied to any filesystem. Btrfs is one filesystem. We work on whatever you've got.

The "why this works for agents" column is the empirical defense. Agent-touched files are source and config and small build artifacts. Kilobytes to low megabytes. A megabyte copy is two hundred microseconds, three orders of magnitude smaller than the LLM call that triggered the edit. Even an enormous node_modules tree only pays the copy cost on the very first write to each file, and most agents only touch a handful of files per branch.
-->

---

# Branch Chain Resolution & Tombstones

<div class="grid grid-cols-2 gap-5 text-sm mt-1">

<div>

### Looking up a file

For `open("/mnt/work/@b3/src/main.py")`:

```text
1. Check @b3's Δ          ← found? serve here
2. Walk ancestors: @b3 → @b1 → base
3. Hit base directory     ← serve here
4. Encounter tombstone?   ← return ENOENT
```

Branches form a chain back to base. Lookups stop at the first hit.

</div>

<div>

### Tombstones handle deletions

Deletion on a branch writes a sentinel:

```text
@b3/Δ/.tomb/src/old.py
```

Without tombstones, deleting a file on a branch would let the base copy "reappear" through the chain on next lookup.

<div class="mt-2 p-2 bg-green-50 rounded border border-green-300 text-xs">
<strong>Portable by construction:</strong> all BranchFS needs from the underlying FS is a writable directory. Branch create = <code>mkdir</code>. Branch destroy = <code>rm -rf</code>.
</div>

</div>

</div>

<!--
Looking up a file in a branch is a chain walk.

On the left, the algorithm. When you open a path under a branch, we check that branch's delta first. If we find the file, we serve it. If not, we walk ancestors (a sub-branch's parent, that parent's parent) until we reach the base directory. We serve the first hit.

The subtle case is deletion. If you delete a file on a branch and we just remove the delta entry, the next lookup would walk back to the base and find the original copy still there. The file would seem to come back. That's wrong.

The fix is on the right: tombstones. When you delete a file on a branch, we write a sentinel under .tomb in the branch's delta. The chain lookup checks for tombstones at every level, and if it sees one, it returns ENOENT instead of falling through.

This is also where the portability story lives. The green box: all BranchFS needs from the underlying filesystem is a directory it can write to. Branch creation is mkdir. Branch destruction is rm -rf. No filesystem-specific operations. ext4 works. NFS works. tmpfs works. The same daemon works on macOS over APFS. That's the answer to "why not just use Btrfs subvolumes", because half your users aren't on Btrfs.
-->


---

# Commit: Atomic Promotion to Parent

<div class="text-sm mt-3">

A commit applies a branch's delta to its parent in six steps:

```text
1. Collect modified files + tombstones from Δ
2. Apply tombstones to parent's Δ      (deletes first)
3. Copy modified files into parent's Δ (then creates)
4. Increment parent's epoch counter    (single atomic op)
5. SIGBUS on siblings' mmap'd regions  (their state is now stale)
6. Sibling's next FUSE op returns -ESTALE
```

</div>

<div class="mt-4 p-2 bg-blue-50 rounded border border-blue-300 text-xs">
Order matters: deletes before creates. Otherwise a delete-then-recreate sequence in the branch could miscompose against an unchanged parent file with the same name.
</div>

<!--
A commit applies a branch's changes to its parent. Six steps, ordered carefully.

Step one: collect the modified files and the tombstones from the committing branch's delta.

Step two: apply the tombstones to the parent's delta. Deletes first. This ordering matters. If the branch did a delete-then-create of a file, doing the create first would leave the file there and then the tombstone would delete it. By doing tombstones first, we preserve the right semantics.

Step three: copy the modified files into the parent's delta.

Step four: increment the parent's epoch counter. This is a single atomic operation. It's the moment of commitment, once this is done, the commit has happened and all siblings are conceptually dead.

Step five: any sibling that has memory-mapped a file from this branch sees its mapping go stale. We deliver SIGBUS on next access so the sibling notices.

Step six: any sibling's next FUSE operation returns -ESTALE. That's the signal to the agent's task wrapper that this branch lost the race and should exit.

The blue note at the bottom is just the rationale for step ordering, deletes before creates avoids a miscomposition bug.
-->

---

# Abort, Epoch Counter, and Costs

<div class="grid grid-cols-2 gap-5 text-sm mt-1">

<div>

### Abort: near-zero cost

- Delete the branch delta
- No base-copy cleanup
- Cost scales with changed files, not workspace size

### First-commit-wins

- Winner commits to the parent
- Siblings become stale
- Next sibling operation returns `-ESTALE`

</div>

<div>

### Costs at a glance

| Op | Cost |
|-----|------|
| Create | ~300 µs (mkdir) |
| Commit 1 KB | ~317 µs |
| Commit 1 MB | ~2.1 ms |
| Abort | ~315 µs |

<div class="mt-3 p-2 bg-blue-50 rounded border border-blue-300 text-xs">
This is BranchFS's <strong>first-commit-wins primitive</strong>. The <code>branch()</code> syscall (later) drives it through ioctls, without changing the semantics.
</div>

</div>

</div>

<!--
Abort and the epoch mechanism that makes first-commit-wins work.

Top left: abort is much simpler than commit. rm -rf the branch's delta directory. That's it. Siblings are untouched. The cost is just the unlink work, proportional to whatever the aborted branch had built up. No coordination needed.

Bottom left: the epoch counter is the trick that makes first-commit-wins work without a global lock. Each branch carries two numbers: the epoch it expects from its parent, and its own current epoch. When you commit, the parent's epoch advances. Any sibling's expected-parent-epoch no longer matches the actual parent epoch. The next FUSE operation from that sibling sees the mismatch and returns -ESTALE. The detection is lazy, which is what we want: we don't want to do anything when a winner commits other than bump a counter. The siblings discover their fate when they next try to do something.

Right: the cost table. Create is 300 microseconds, dominated by the mkdir of the delta directory. Commit cost scales with modification size: a kilobyte is 317 microseconds, a megabyte is two milliseconds. Abort is roughly constant (315 microseconds) because it's just the unlink work. All numbers from a small Ryzen 5500U laptop.

The blue box is the foreshadowing line. The branch() kernel syscall, which we'll see in a few slides, drives these exact commit and abort operations through ioctls. The semantics don't change. The kernel just adds atomic process coordination on top.
-->


---

# Performance: Branch, Commit, Abort

<div class="grid grid-cols-2 gap-4 text-sm mt-3">

<div class="border-2 border-blue-400 rounded-lg p-4">

<div class="font-semibold text-blue-600 mb-2">Branch Creation: O(1)</div>

| Base Size | Latency |
|:-:|:-:|
| 100 files | 292 μs |
| 1,000 files | 317 μs |
| 10,000 files | 310 μs |

Independent of base size, it's just a `mkdir`.

</div>

<div class="border-2 border-green-400 rounded-lg p-4">

<div class="font-semibold text-green-600 mb-2">Commit & Abort</div>

| Mod. Size | Commit | Abort |
|:-:|:-:|:-:|
| 1 KB | 317 μs | 315 μs |
| 100 KB | 514 μs | 365 μs |
| 1 MB | 2.1 ms | 890 μs |

Proportional to modification size, not workspace size.

</div>

</div>

<div class="mt-4 text-sm text-center">

For agents doing LLM calls of **100 ms – 10 s** per step, **sub-millisecond branching is invisible**.

</div>

<div class="mt-3 text-xs text-center opacity-60">
Hardware: AMD Ryzen 5 5500U (6c/12t), 8 GB DDR4, NVMe SSD. Median of 10 trials.
</div>

<!--
Two numbers worth knowing about the branch lifecycle.

Left box: branch creation is O(1). It does not matter whether your base directory has a hundred files or ten thousand files; creating a branch costs about 300 microseconds. That's because branch creation is literally a mkdir of the delta directory. No file copying happens until you actually write something.

Right box: commit and abort scale with how much you changed, not how big the workspace is. Committing a kilobyte of changes is 317 microseconds. A megabyte is two milliseconds. Abort is even cheaper than commit because it just unlinks; abort is fundamentally O(delta size).

The line under the boxes is the framing I want you to walk out with. Agents do LLM calls. Those calls take 100 milliseconds at the absolute fastest, often several seconds. Sub-millisecond branching is invisible against that. We're not even close to being the bottleneck.

The next slide tackles the question I get every time I talk about a FUSE filesystem: but isn't FUSE slow?
-->

---

# Performance: FUSE Read Throughput

<div class="grid grid-cols-2 gap-5 text-sm mt-3">

<div>

| Mode | Read |
|:-:|:-:|
| Native ext4 | 8.8 GB/s |
| FUSE (default) | 1.7 GB/s |
| **FUSE passthrough** | **7.2 GB/s** |

**82% of native** with FUSE 3 passthrough.

50 MB file, 64 KB blocks.

</div>

<div>

### What is FUSE 3 passthrough?

- Added in mainline kernel **6.9**
- Daemon hands the kernel a lower-FD
- Subsequent reads bypass the daemon entirely
- Used by BranchFS for **all unmodified files**
- Modified files still go through the daemon

<div class="mt-3 p-2 bg-blue-50 rounded border border-blue-300 text-xs">
The "FUSE is slow" reputation comes from the default mode's 19% number. Passthrough closes the gap to ~82% with no application changes.
</div>

</div>

</div>

<!--
The FUSE performance slide.

Left table: native ext4 on this NVMe drive reads at 8.8 gigabytes per second on a 50 MB file. BranchFS in default FUSE mode is 1.7 GB/s, that's the 19% number that gets thrown at people to argue FUSE is too slow for anything serious. The default FUSE path bounces every read through a kernel-to-userspace context switch.

But: FUSE 3 added passthrough mode in kernel 6.9. Passthrough is exactly what it sounds like, the daemon registers the lower file descriptor with the kernel, and from then on, reads to the upper file go straight to the lower one without round-tripping through the daemon. We use this for all unmodified files. Modified files still go through the daemon because we need to serve from the delta copy.

With passthrough, BranchFS reads at 7.2 gigabytes a second, 82% of native. That's the right number to remember if someone tells you FUSE is too slow.

The blue note is just for the audience who knows FUSE well: yes, the bad reputation is real, and yes, passthrough fixes most of it. The gap remaining (about 18%) is still kernel-to-userspace bookkeeping that we haven't optimized. For agent workloads it's irrelevant.
-->


---

# Demo: A Parallel Agent Run, Start to Finish

<div class="grid grid-cols-2 gap-5 text-sm mt-3">

<div>

### Transcript

```text
$ branchfs mount $PWD /mnt/work

$ branchctl create /mnt/work fix-a fix-b fix-c
@fix-a  @fix-b  @fix-c

$ run-agent @fix-a &
$ run-agent @fix-b &
$ run-agent @fix-c &
$ wait

$ grep -l "passed" *.log
b.log

$ branchctl commit /mnt/work/@fix-b
[branchfs] committed @fix-b -> base
[branchfs] invalidated @fix-a, @fix-c
```

</div>

<div>

### What happened

- Three candidate fixes ran in parallel
- Each branch wrote to its own delta
- `@fix-b` committed atomically
- Losing branches became stale
- Disk cost: changed files, not workspace copies

<div class="mt-5 p-3 bg-green-50 rounded border border-green-300 text-base">
This is the whole fork-explore-commit loop in userspace today.
</div>

</div>

</div>

<!--
Let's walk through what this actually looks like end to end. This is a transcript from a real run on my laptop, slightly trimmed for the slide.

We mount BranchFS over the current directory with a base directory that lives in /tmp. The daemon starts up, reports that FUSE 3 passthrough is enabled.

We create three named branches: fix-a, fix-b, fix-c. They show up as @-prefixed paths under the mount.

Now we launch three agent runs in parallel. Each one cd's into its own @-branch and runs the same task: "fix the off-by-one." They run concurrently. Each one independently modifies its own delta, runs its own tests, produces its own log.

We grep for which one's tests passed. fix-b won.

We commit fix-b. BranchFS reports: epoch went from zero to one, siblings invalidated. The changes from fix-b are now in the base directory. The next agent who opens the workspace sees fix-b's fix.

We clean up the losers with branchctl abort. They're gone, no files left behind, no /tmp clutter.

The thing I want you to notice is the disk footprint. Three branches running in parallel did not require three copies of the workspace. Each branch's delta only contains the files it actually modified. If the off-by-one was a one-line change to one Python file, each delta is a few hundred bytes. We just got three-way exploration for the cost of three small files, not three copies of a multi-gigabyte repo.

This is BranchFS working in userspace today. No kernel changes required. This script runs on Ubuntu 22.04, on Fedora, on Arch, on a Mac with macFUSE, wherever you have FUSE 3.
-->

---

# Why a Syscall? Userspace Is Not Enough

<div class="text-sm mt-3">

### What BranchFS gives us today

| Requirement | Status |
|------------|--------|
| R1 isolated views | ✓ via delta layers |
| R2 atomic commit | ✓ via epoch counter |
| R3 nesting | ✓ via branch chain |
| R4 complete FS coverage | ✓ |
| R5 unprivileged, portable | ✓ |
| R6 process coordination | **✗, userspace cannot do this safely** |

</div>

<div class="mt-4 p-3 bg-red-50 rounded border border-red-300 text-sm text-center">
Five of six checked. R6 needs the kernel, and that's the next slide.
</div>

<!--
Pause and stock-take. Look at the requirements table.

R1 through R5: BranchFS in userspace gives us all of them. Isolated views via delta layers. Atomic commit via the epoch counter. Nesting via the branch chain. Complete FS coverage because we intercept everything at the FUSE layer. Unprivileged and portable because we're just a userspace FUSE daemon over an ordinary directory.

R6, process coordination, is the one we cannot get from a userspace FUSE filesystem alone. We need atomic process spawn into a branch context, reliable termination of all processes in a branch when it commits or aborts, a fence between siblings so they can't signal each other, and we need all of that to compose atomically with the FS branch setup so there are no race windows. That's the kernel's territory.

The red box is the segue: five of six checked, the last one needs the kernel. The next slide shows you what userspace can and can't do for process coordination, and the slide after that shows the kernel race in code.
-->

---

# What the Kernel Has to Do

<div class="text-sm mt-3">

Five capabilities `branch()` provides, only two are even *possible* from userspace:

| Capability | Userspace? |
|-----------|-----------|
| **Atomic composition** of FS branch + mount NS + process group | Impossible, race windows |
| **Memory branching** (page-table CoW for `BR_MEMORY`) | Impossible, kernel only |
| **Reliable process termination** of all branch descendants | Cgroups only, needs root |
| **Sibling signal/ptrace fence** (siblings can't `kill -9` each other) | PID ns only, PID 1 overhead |
| **Atomic mount-namespace setup** with `open_tree` + `move_mount` | Possible but fragile |

</div>

<div class="mt-4 p-3 bg-blue-50 rounded border border-blue-300 text-base text-center">
One syscall composes all five atomically, with kernel-side cleanup on partial failure.
</div>

<!--
The capabilities we need fall into two groups.

Left: the table. Two capabilities are impossible from userspace, full stop. Atomic composition: there's no userspace mechanism that turns multiple kernel operations into one atomic operation. Memory branching, page-table copy-on-write, is a thing only the kernel can do. The other three, reliable termination, sibling fences, atomic mount setup, are technically possible in userspace, but only with privileged operations, with race-prone multi-step sequences, or with PID-namespace overhead that defeats R5.

Right: the four things the kernel needs to make atomic. Atomic composition of FS branch, mount namespace, process group, and sibling fence, that's the headline. Memory branching, which is page-table CoW we don't need today but will want once agents start checkpointing in-process state. Reliable termination, where cgroups get you most of the way today but need root. Sibling fences, where PID namespaces work but bring PID-1 overhead. And atomic mount setup, the least exotic of the bunch but still finicky to drive from userspace because of the new mount API.

The blue box is the spoiler. We propose branch(), one syscall that composes all of this atomically. The next slide is its interface.
-->


---

# The `branch()` Syscall: Interface

<div class="text-sm mt-3">

```c
long branch(int op, union branch_attr *attr, size_t size);
```

</div>

<div class="grid grid-cols-2 gap-5 text-sm mt-4">

<div>

### Three operations

| op | who calls it |
|----|--------------|
| `BR_CREATE` | parent: fork N children, each in its own branch |
| `BR_COMMIT` | child: apply this branch to parent, kill siblings |
| `BR_ABORT` | child: discard this branch |

`bpf(2)`-style multiplexed union → ABI-extensible.

</div>

<div>

### Composable flags for `BR_CREATE`

| Flag | Effect |
|------|--------|
| `BR_FS` | Mount namespace + FS branch (required) |
| `BR_MEMORY` | Page-table CoW for memory |
| `BR_ISOLATE` | Signal/ptrace fence between siblings |
| `BR_CLOSE_FDS` | Close inherited FDs |

</div>

</div>

<!--
The proposed syscall. The shape is deliberately small.

One entry point: branch. Three arguments: an operation code, a pointer to a union of per-operation argument structs, and the size of that union, that's the bpf(2)-style trick so we can extend the union later without breaking ABI.

Left side: three operations. BR_CREATE is called by the parent and creates N branches plus forks N children. BR_COMMIT is called by a child and commits its branch to the parent, terminating the siblings. BR_ABORT is called by a child and discards the branch.

Right side: four composable flags on BR_CREATE that control which resources are branched. BR_FS is required: that gives you the mount namespace and the BranchFS branch. BR_MEMORY adds page-table copy-on-write of memory; we'll come back to this. BR_ISOLATE installs a kernel-enforced signal and ptrace fence between siblings: they cannot signal each other, they cannot ptrace each other, even if they're the same user. BR_CLOSE_FDS closes inherited file descriptors so the children re-open in their branch context.

Three operations, four flags. That's the entire surface area. The next slide shows you what calling it looks like.
-->

---

# The `branch()` Syscall: Usage

<div class="text-xs mt-1">

```c
int mnt = open("/mnt/work", O_PATH);
pid_t pids[3];
union branch_attr a = {
  .create = {
    .flags = BR_FS,
    .mount_fd = mnt,
    .n_branches = 3,
    .child_pids = (uintptr_t)pids,
  }
};
int idx = branch(BR_CREATE, &a, sizeof(a));

if (idx == 0) {
  // parent: wait for winner
  while (wait(NULL) > 0);
} else {
  // child: idx is 1, 2, or 3
  if (try_fix(idx)) {
    union branch_attr c = {.commit = {0}};
    int r = branch(BR_COMMIT, &c, sizeof(c));
    if (r == -ESTALE) _exit(1);  // lost the race
  } else {
    union branch_attr ab = {.abort = {0}};
    branch(BR_ABORT, &ab, sizeof(ab));
  }
}
```

</div>

<div class="mt-3 p-2 bg-blue-50 rounded border border-blue-300 text-sm">
One syscall returns different values to parent (0) and to each child (1..N). The race between commits is resolved <strong>atomically inside the kernel</strong>, losers get <code>-ESTALE</code>.
</div>

<!--
What calling branch() actually looks like, end to end.

The parent opens the workspace as an O_PATH fd. It fills in a branch_attr with the flags (BR_FS) the mount fd, the branch count 3, and an output buffer for the child PIDs. Then it calls branch(BR_CREATE).

The syscall returns. To the parent, it returns 0 and the pids array is filled in. To each of the three children, it returns 1, 2, or 3, that's their branch index. So with one syscall, the parent and the children are now running in different processes, in different mount namespaces, looking at different deltas, all with the right values in idx.

In each child, the application tries its fix. If try_fix returns true, the child calls BR_COMMIT. The kernel does the atomic compare-and-swap on the winner field. If you won, the syscall returns 0 and your changes are now in the parent and your siblings are dead. If you lost (because some other sibling committed first) the syscall returns -ESTALE and you exit.

If the try_fix failed, the child calls BR_ABORT. The syscall discards the branch and terminates the child.

The blue box is the key property to remember. One syscall, different return values to parent and children. The commit race is resolved atomically inside the kernel. Losers get a clear -ESTALE return code. No userspace coordination needed at all.
-->


---

# Filesystem-Agnostic via Generic ioctls

<div class="grid grid-cols-2 gap-5 text-sm mt-1">

<div>

### The contract

`branch()` does **no FS-specific work**. It delegates storage operations to the mounted branching filesystem:

```c
FS_IOC_BRANCH_CREATE  _IO('b', 0)
FS_IOC_BRANCH_COMMIT  _IO('b', 1)
FS_IOC_BRANCH_ABORT   _IO('b', 2)
```

### What this means

- **BranchFS** implements these ioctls today
- Other filesystems can implement the same contract later
- New backend = storage semantics, not a new syscall

</div>

<div>

### Flow on `BR_CREATE`

```text
                 userspace
       branch(BR_CREATE, n=3, mount_fd)
                    │
       ────────── kernel ──────────
                    │
       ┌────────────┼────────────┐
       ▼            ▼            ▼
  vfs_ioctl    vfs_ioctl    vfs_ioctl
  CREATE       CREATE       CREATE
       │            │            │
       ────── FUSE crossing ──────
                    │
              BranchFS daemon
              (allocate Δ₁, Δ₂, Δ₃)
                    │
       ────── kernel resumes ──────
                    │
              fork × 3
                    │
       inject branch_id into child's
       pt_regs->ax so syscall returns
       1, 2, 3 to each child
```

</div>

</div>

<!--
Before we look at the kernel internals, a design choice that matters for the wider ecosystem.

The branch() syscall does no filesystem-specific work. None of the branch lookup, none of the delta management, none of the commit logic lives in the kernel. The syscall talks to whatever branching filesystem you have mounted via three generic ioctls: FS_IOC_BRANCH_CREATE, COMMIT, and ABORT.

This follows the existing pattern of generic ioctls in Linux that any filesystem can implement, FICLONE for cross-filesystem clones, FIEMAP for getting extent maps, FIDEDUPERANGE for deduplication. These aren't tied to any one filesystem; any FS that wants to support the operation implements the ioctl.

What this gets us is plug-ability. BranchFS implements these three ioctls in its FUSE daemon today, so the syscall works against BranchFS out of the box. But there is nothing stopping someone from implementing them in a future Btrfs branching mode that uses real Btrfs subvolumes. Or in an OverlayFS commit mode. Adding a new branching filesystem to the ecosystem requires implementing three ioctls. No syscall changes, no VFS changes.

The diagram on the right walks through what happens on BR_CREATE in the prototype. Userspace calls branch(BR_CREATE, n=3, mount_fd). The kernel issues vfs_ioctl(FS_IOC_BRANCH_CREATE) three times against the mount fd. Each call goes through the FUSE protocol to the BranchFS daemon, which allocates a delta directory and returns the branch name. Then the kernel forks three children. For each child, we inject the branch_id into the child's pt_regs (specifically into the ax register on x86_64) so when the child returns from the syscall, it returns 1, 2, or 3 instead of 0. That gives us the "single syscall, different return values in parent and children" semantics that the userspace code expects.
-->

---

# Working Prototype: Vanilla Linux 6.17 + 3 Patches

<div class="grid grid-cols-2 gap-5 text-sm mt-1">

<div>

### The patch series

```text
prototype/patches/
├── 0001-branch-add-UAPI-and-internal-headers.patch
├── 0002-branch-implement-syscall-and-ioctls.patch
└── 0003-branch-hook-copy_process-and-do_exit.patch
```

Three logical commits against `v6.17`.

### Five-minute build

```bash
$ ./scripts/build-kernel.sh   # clone + patch + build
$ cargo build --release       # BranchFS
$ make -C test                # test program
$ ./scripts/build-rootfs.sh   # initramfs
$ ./scripts/run-qemu.sh       # boot + run
```

</div>

<div>

### Capability status

| Item | Status |
|------|--------|
| `BR_CREATE` / `BR_COMMIT` / `BR_ABORT` | ✅ |
| CAS-based first-commit-wins | ✅ |
| Sibling SIGKILL | ✅ |
| `FS_IOC_BRANCH_*` ↔ BranchFS | ✅ |
| `BR_ISOLATE` fence | ⚠️ flag accepted, fence not plumbed |
| Mount NS + bind-mount setup | ❌ children `chdir(@<name>)` |
| `BR_MEMORY` | ❌ returns `-EOPNOTSUPP` |
| Nested branches | ❌ `-EBUSY` |

</div>

</div>

<!--
The prototype.

Left side, top: the patch series is three git format-patch commits against vanilla Linux 6.17. The first adds the UAPI headers and internal kernel headers, types only, no logic. The second implements the syscall body and wires up the ioctl path to BranchFS. The third adds the two kernel hooks, into copy_process and do_exit, and adds the new fields to task_struct that those hooks need.

Left side, bottom: end-to-end build and test in five minutes on a 24-core machine. build-kernel.sh clones 6.17, applies the three patches, builds. cargo builds BranchFS. make builds the C test program. build-rootfs packs everything into an initramfs. run-qemu boots the patched kernel under QEMU with KVM, runs the tests, powers off. The repo is on GitHub; you can do this tonight.

Right side: the honest status table. The three core operations (create, commit, abort) work. First-commit-wins works. Sibling SIGKILL works. The bridge to BranchFS through the FS_IOC_BRANCH_* ioctls works. The BR_ISOLATE flag is accepted but the kernel-side fence is not plumbed in yet, that's a few hundred lines in kernel/signal.c and kernel/ptrace.c. Mount-namespace setup with bind-mounting is deferred; for now, children chdir into their @-branch directory themselves. BR_MEMORY returns -EOPNOTSUPP. Nested branches return -EBUSY. Those last three are the next round of work.

The QEMU test harness exercises all of this (single commit, three-way race with sibling SIGKILL, abort, and a latency micro-bench) and prints rc=0x0 at the end. No oopses, no warnings. Real code, not slideware. The next slide is what the latency micro-bench measures.
-->

---

# Latency: Syscall Path Is Cheap

<div class="grid grid-cols-2 gap-6 text-base mt-4">

<div>

### Measured in QEMU/KVM

<div class="space-y-4 mt-4">

<div class="p-4 bg-blue-50 rounded border border-blue-300">
<div class="text-3xl font-semibold">61–70 µs</div>
<div><code>BR_CREATE</code>: create branch state + fork child</div>
</div>

<div class="p-4 bg-green-50 rounded border border-green-300">
<div class="text-3xl font-semibold">12–25 µs</div>
<div><code>BR_COMMIT</code>: CAS + filesystem commit path</div>
</div>

<div class="p-4 bg-gray-50 rounded border border-gray-300">
<div class="text-3xl font-semibold">~300 µs</div>
<div>BranchFS CLI branch creation in the paper</div>
</div>

</div>

</div>

<div>

### The big picture

<div class="mt-5 text-xl leading-loose">

- LLM/tool step: **100 ms – 10 s**
- `branch(BR_CREATE)`: **~70 µs**
- Ratio: **at least 1000:1**

</div>

<div class="mt-6 p-4 bg-blue-50 rounded border border-blue-300 text-base leading-relaxed">
These are sanity-check prototype numbers, not final benchmarks. The point is scale: branching is far below the latency of an agent step.
</div>

</div>

</div>

<!--
The fourth test prints latency numbers. Let's look at what they say.

Left side. Steady-state, after the cold-cache first iteration, BR_CREATE takes 61 to 70 microseconds on the parent side. That's the full round-trip: one vfs_ioctl to BranchFS through FUSE, one kernel_clone, the fork_inherit hook, the pt_regs override, the copy_to_user of the child PIDs back to the parent. 70 microseconds end to end.

BR_COMMIT on the child side, steady state, is 12 to 25 microseconds. That's the atomic CAS plus the vfs_ioctl to BranchFS for the commit, plus sibling cleanup which is zero work for N=1.

The paper has BranchFS branch creation at about 300 microseconds. We're four times faster than the paper. Why? The paper measures from the branchctl CLI invocation; that includes process startup, argument parsing, the user-side CLI talking to the daemon over a socket. The kernel syscall path bypasses all of that, it talks directly to the FUSE daemon via the ioctl.

Right side, the cost breakdown of BR_CREATE. The vfs_ioctl path to BranchFS is the largest single chunk, about 28 microseconds, which is dominated by the FUSE protocol round-trip, the kernel-to-userspace context switch. kernel_clone is about 25 microseconds. Our own hook is about 12 microseconds. There's about 5 microseconds of bookkeeping. None of those are anywhere near the size of a Python import, let alone an LLM call.

I want to be honest in the highlighted box: these are sanity-check numbers from a 4-vCPU QEMU guest. They tell you the prototype isn't doing anything pathological. Paper-quality numbers would want bare metal, multiple base sizes, p99 distributions, and a head-to-head against the closest userspace equivalent, something like unshare plus overlayfs plus a shell script. That work is on the to-do list and would be a nice OSSummit talk in itself.

The most important number on this slide is the ratio at the bottom right. The LLM step the agent is doing is between 100 milliseconds and 10 seconds. branch() is 70 microseconds. That's at least three orders of magnitude headroom. Branching is in the noise of agent workloads.
-->

---

# The Python Library: BranchContext

<div class="grid grid-cols-2 gap-5 text-base mt-2">

<div>

### Patterns the library exposes

| Pattern | Strategy |
|---------|----------|
| **BestOfN** | Run N, commit highest-scoring |
| **Speculate** | Race candidates, first success wins |
| **TreeOfThoughts** | Hierarchical nested branches |

Each pattern manages branch creation, scoring, commit, and cleanup.

`github.com/multikernel/branching`

</div>

<div>

### What an agent author writes

```python
from branchcontext import BranchContext

ctx = BranchContext("/mnt/work")

# Best-of-N: try 3 fixes, commit best
result = ctx.best_of_n(
    n=3,
    task=lambda b: try_fix(b),
    score=lambda b: run_tests(b),
)
# Winner is already committed.
# Losers' deltas are gone.
```

<div class="mt-3 p-3 bg-green-50 rounded border border-green-300 text-base">
Same Python API: BranchFS today, <code>branch()</code> later.
</div>

</div>

</div>

<!--
Most people won't write to the syscall directly. They'll use a library. We ship one, for Python, because that's where agents live today.

BranchContext is a Python library that wraps BranchFS's primitives into seven exploration patterns, on the left. Speculate races N candidates and commits the first success. BestOfN runs all N and commits the highest-scoring. Reflexion does sequential retry where each retry sees the previous failure's feedback. TreeOfThoughts is hierarchical, with nested branches. BeamSearch keeps top-K at each depth level. Tournament does pairwise elimination via a judge function. Cascaded starts with one branch and adaptively fans out on failure.

Each of those patterns is maybe a hundred lines of Python around the BranchFS primitives. They handle branch creation, the parallel execution, the success/failure judging, and cleanup.

What the agent author writes is on the right. Open a BranchContext over the BranchFS mount. Call best_of_n with N=3, a per-branch task function, and a scoring function. That's it. The library creates the branches, runs the tasks in parallel, scores each one, commits the best, discards the rest. The agent author writes lambdas, not subprocess plumbing.

The thing in the highlighted box is the migration story. Today, BranchContext calls BranchFS via its CLI and ioctls. When the branch() syscall is upstream, the library can switch to using the syscall, same Python API, more atomic underneath. That's deliberate. We want agent authors to write to a stable API now and benefit from kernel improvements later without rewriting.

Today: pure userspace. pip install branchcontext. You need BranchFS mounted. It works on macOS too via macFUSE.
-->

---

# Status: What's Shipping and What's Not

<div class="grid grid-cols-2 gap-5 text-sm mt-1">

<div>

### Shipping today

- **BranchFS**: production-quality FUSE filesystem, Linux + macOS
- **BranchContext**: Python library, 7 patterns, on PyPI
- **branch() prototype**: 3 patches against v6.17, QEMU test harness, passing tests

</div>

<div>

### Honest limitations

- **External side effects** (network, IPC) not rolled back on abort
- **Single-winner only**: no multi-branch merge
- **File-level CoW**: symlinks, hardlinks, FIFOs partial
- **`BR_MEMORY`** deferred, page-table CoW is real mm work
- **Nested branches** deferred in kernel prototype

</div>

</div>

<!--
Where we are.

Left side: what's shipping today, honestly. BranchFS is production-quality. People have it running on Linux laptops, in CI, and on macOS via macFUSE. BranchContext, the Python library, is on PyPI; pip install gets you a working setup. The branch() prototype is in our repo with the three patches, the QEMU test harness, and a passing test run. None of this is slideware.

Right side: the limitations I want to call out before someone asks.

External side effects (network calls, IPC, anything that escapes the filesystem) are not rolled back on abort. If a branch sent an email, the email's gone. We don't have effect gating yet; that's a research direction we'll touch on next slide.

Single-winner only. There's no multi-branch merge. If you wanted to combine non-overlapping changes from two branches, you'd have to do it yourself outside the framework.

File-level CoW has partial support for the trickier file types, symlinks targeting absolute paths outside the branch, hardlinks losing their link relationship on first write, FIFOs and sockets and device nodes in delta layers. These suffice for agent workloads but you'd want them fixed for general use.

BR_MEMORY is deferred. Page-table copy-on-write is real memory-management work; not hard, but a few weeks of careful code. We've architected for it but not implemented it.

Nested branches are deferred in the kernel prototype, though the BranchFS branch chain already handles them. The kernel side needs to lift one guard and that's largely it.

Next slide: where we're going.
-->

---

# Roadmap

<div class="grid grid-cols-2 gap-6 text-base mt-4">

<div>

<div class="border-l-4 border-blue-500 pl-3 mb-3">

**Mainline RFC**: port the `branch()` prototype forward and send the first patch series.
</div>

<div class="border-l-4 border-green-500 pl-3 mb-3">

**Sibling isolation**: finish the signal / ptrace fence for hostile or buggy branches.
</div>

<div class="border-l-4 border-orange-500 pl-3 mb-3">

**Nested branches**: complete kernel support for recursive exploration.
</div>

<div class="border-l-4 border-purple-500 pl-3">

**Effect gating**: buffer network / IPC until commit.
</div>

</div>

<div>

### Beyond agents

With `n_branches=1`, `branch()` is also a generic **try-and-rollback** primitive:

- Package upgrades: try, abort if broken
- System config: try, revert if reboot fails
- Schema migrations: try, roll back on error

<div class="mt-5 p-3 bg-blue-50 rounded border border-blue-300 text-base">
The fork-explore-commit lifecycle is more general than agents. Agents are just the loudest current use case.
</div>

</div>

</div>

<!--
Roadmap. Six months out.

Left column, four items in priority order.

One: port the prototype forward to current mainline, prepare an RFC patch series, send it to linux-kernel. The two hooks are small enough that we think this is actually upstreamable. If you're a kernel reviewer and want to look at it before that lands, please come find us afterward.

Two: implement the BR_ISOLATE fence. A few hundred lines in kernel/signal.c and kernel/ptrace.c. This makes sibling isolation safe against hostile or buggy siblings, not just polite ones.

Three: nested branches in the kernel. BranchFS's chain already handles arbitrary depth. The kernel side just needs to lift the current->branch != NULL guard and handle the parent-of-parent commit semantics. Lift-and-shift work.

Four: effect gating. This is the bigger research direction, buffering network and IPC until commit, discarding on abort. Agent gateways like Agentry mediate all agent-to-external communication and are a natural interposition point.

Right column is the bonus direction. With n_branches set to 1, branch() is a generic try-and-rollback primitive. Package upgrades: try, abort if broken. System config changes: try, revert if the reboot fails. Schema migrations: try, roll back on error. Anywhere you have the shape "do a thing and undo it cleanly if it's bad," branch() is a Linux-native way to do it.

The blue note is the framing line: agents are the loudest current use case but the abstraction is more general.
-->


---

# How to Try It, How to Help

<div class="grid grid-cols-2 gap-4 text-xs mt-1">

<div>

### Try it

```bash
# BranchFS — works on any Linux today
$ cargo install branchfs && branchfs mount /repo /mnt/work
$ pip install branchcontext
```

Kernel prototype + paper: <https://arxiv.org/abs/2602.08199>

### Help wanted

- **Bugs / features** → GitHub issues
- **Kernel review** → LKML thread (soon)
- **New FS backends** → implement `FS_IOC_BRANCH_*`
- **Agent integrations** → BranchContext patterns

</div>

<div>

### Repos

- BranchFS — `github.com/multikernel/branchfs`
- BranchContext — `github.com/multikernel/branching`
- Paper — <https://arxiv.org/abs/2602.08199>

</div>

</div>

<!--
Concretely, how do you try this and how do you help.

Try it. Left column. Cargo install branchfs gives you the userspace daemon. Mount it over any directory. Pip install branchcontext gives you the Python library. If you want to play with the kernel prototype, clone our repo, run two scripts, you have a patched kernel running in QEMU in about five minutes. Everything is open and reproducible.

Help. We're open to four kinds of contributions. Bugs and features on GitHub. Kernel review, which is going to start happening on linux-kernel once we have the patches forward-ported. New branching filesystem backends: if you want to make Btrfs grow native branching, the contract is the three FS_IOC_BRANCH_* ioctls. And agent integrations: new exploration patterns in BranchContext, or wrappers for languages other than Python.

Right side. The repos. BranchFS, BranchContext, and the paper-plus-prototype repository. Licenses are conservative: userspace is dual MIT/Apache-2.0, the kernel patches are obviously GPL-2.0.

Our contact info, please reach out. We are genuinely interested in talking to people who want to use this in production or who have skeptical questions about the design.
-->

---

# Key Takeaways

<div class="text-xl leading-relaxed mt-8">

1. **AI agents are ordinary Linux processes with extraordinary side effects.** The OS and Sandbox cannot tell when a process is one speculative path among many.

2. **Existing primitives don't compose atomically.** Filesystem branching, namespaces, cgroups, and signal fences need one kernel-level lifecycle.

3. **Branch context = CoW filesystem view + confined process group.** Fork, explore, commit; first winner lands, losers disappear.

4. **BranchFS works today; `branch()` is the kernel path.** Userspace prototype now, RFC direction next.

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
Five things to walk out with.

One: AI agents are ordinary Linux processes with extraordinary side effects. They look like normal Unix workloads to the kernel, but they generate filesystem and process state at a rate and in a pattern that the existing tools don't handle gracefully. The OS has no abstraction for "this is one of N speculative paths." That's the gap.

Two: existing primitives don't compose. OverlayFS gets you isolation but not commit. Btrfs gets you snapshots but not portability. Namespaces and cgroups get you process isolation but require root and have race windows when you try to stitch them together. The composition is the problem, not any individual piece.

Three: branch context is the abstraction we're proposing. Copy-on-write filesystem view plus confined process group. Fork, explore, commit. First-commit-wins. Nestable. Small surface area.

Four: BranchFS (the userspace half) works today. FUSE 3, Rust, no root, portable. The performance is fine for agent workloads, and FUSE 3 passthrough mode closes the gap for sustained workloads.

Five: the branch() syscall (the kernel half) is a small patch. Two hooks, three ioctls, three patches against v6.17. We have a working prototype that passes tests in QEMU. An RFC to linux-kernel is in our short-term roadmap.

If you've been trying to wrangle agents and you've found yourself reaching for cp -r or for Docker, please go look at the repos at the bottom. And come find us afterward, we'd love to hear what would or wouldn't fit your use case.
-->

---
layout: center
class: text-center
---

# Thank You – Questions?

<div class="mt-6 text-lg">

**Cong Wang** – cwang@multikernel.io

**Yusheng Zheng** – yzhen165@ucsc.edu

</div>

<div class="mt-4 text-base opacity-70">
Open Source Summit 2026
</div>

<div class="mt-3 flex gap-6 justify-center text-sm">

<div>BranchFS: github.com/multikernel/branchfs</div>
<div>BranchContext: github.com/multikernel/branching</div>

</div>

<!--
Thank you. Happy to take questions.

Quick prompts in case the room is shy: people often ask about why FUSE instead of a kernel FS, and I'm happy to go deeper. They ask about how this compares to running each agent in its own container, which is a great comparison to draw out. They ask about whether branch() is on the path to mainline: short answer, yes, that's the plan, and we'd value reviewer eyes. And they ask about non-agent use cases: package management, schema migrations, anything where you'd like a try-and-rollback primitive that doesn't require a whole VM.
-->
