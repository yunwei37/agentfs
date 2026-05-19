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
Good morning everyone. Today I want to talk about a problem that shows up once AI agents stop trying one path at a time and start exploring several possible fixes in parallel. We have been building two Linux pieces for that problem: a userspace filesystem called BranchFS, and a proposed kernel syscall called branch(). Together, they give agents a clean way to fork a workspace, explore a path, and either commit it or throw it away.

This is joint work between Multikernel Technologies and eunomia-bpf. The userspace part is open source and works on Linux today. The kernel part is still a prototype, but it is a real patch series against vanilla Linux 6.17.

Over the next thirty minutes, I will first ground this in what an AI agent actually looks like to Linux. If you have not been following the agent space closely, the workload is more ordinary than the hype suggests. Then I will walk through why the tools Linux already has, like OverlayFS, Btrfs, namespaces, and cgroups, each get part of the way there but do not quite fit. After that I will show BranchFS, the FUSE filesystem we wrote for the userspace half. Finally, I will show the branch() prototype, the QEMU tests, and the path we think could make sense upstream.

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
Here is the plan for the talk. I will start with the background, because at the Linux level an agent is just a normal process doing normal filesystem operations. The problem is that those normal operations become speculative side effects once the agent explores more than one path.

From there, we will turn the problem into requirements, then into the branch context design. The implementation has two parts: BranchFS in userspace and branch() in the kernel. At the end, I will show the latency numbers, a demo, the current limits, and the roadmap.
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

</div>

</div>

<!--
Let me start with what I mean by an AI agent. For this talk, an agent is a loop. It asks a model what to do, acts in a local workspace, observes what happened, and then repeats. Most of the time, the action is simple: run a shell command, edit a file, run a test, or apply a patch.

The tools you have heard of mostly fit this shape. Claude Code, SWE-agent, and OpenHands edit a repository and run tests. Aider and Cursor agents do the same thing inside an editor. Devin and the Codex CLI do it on hosted machines. So from the operating system's point of view, they are processes that read files, write files, and spawn other processes.

The interesting part starts when the agent does not try just one path. It starts to fork the work.
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
This is the pattern that motivates the rest of the talk. Agents are starting to try several paths at the same time, instead of betting everything on one attempt. You see this in Best-of-N, Tree-of-Thoughts, RL rollouts, and speculative execution.

For example, an agent might try three candidate bug fixes in the same repository. Each attempt edits files, runs tests, and produces a result. The orchestrator keeps the fix that passes and throws away the other two. For an RL rollout, the idea is similar: run several trials, give each outcome a score or reward, and use that score to decide what to keep.

The names are less important than the shape. The agent fans out into several attempts, lets them run independently, keeps the result that works, and discards the rest. Once those attempts touch the same workspace, they need isolation.
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
Now look at the same pattern from Linux's point of view. If you run strace on one of these tools, you see the same calls you would see from a developer at a terminal: execve for a shell, openat and write for source files, git apply, npm install, unlink. The agent is mostly doing normal Unix work, just much faster than a human.

The process is ordinary, but the side effects are real. It can leave a dirty working tree, install packages, change dotfiles, and leave build output behind. Linux does not know that any of this is speculative. It does not know that this process is only one of several paths being tried.

That is the gap we are trying to fill.
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
Before we look at existing mechanisms, let me spell out what we need. There are five requirements that I will keep coming back to.

First, the branches have to run in parallel without stepping on each other. They may edit the same files, so each branch needs its own view of the workspace.

Second, the model has to support nesting. Patterns like Tree-of-Thoughts can recurse, so a branch may need to create sub-branches of its own.

Third, we need complete filesystem coverage. This is where git stash breaks down. Agents run npm install, pip install, cargo build, and similar commands. Many of the files that matter are ignored by Git, but they still affect the result, so we have to capture them.

Fourth, the mechanism has to be light, unprivileged, and portable. Creating a branch should be cheap enough that an agent can do it during normal reasoning. It should not require root, because agents run in CI, containers, and developer laptops. It also has to work across normal filesystems, not only on Btrfs.

Finally, we need process coordination. Each branch may start a test runner, a compiler, or an installer. When a branch commits or aborts, those processes need to be cleaned up reliably, and one branch should not be able to signal another branch's processes.

With that rubric in mind, let's look at the current options.
-->

---

# Existing Solutions Fall Short

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

### What we actually need:

- **One namespace** the agent lives in
- **N isolated branches** of it
- **Nested branches** for recursive exploration
- **No root**, **portable**

<div class="mt-5 p-4 bg-yellow-50 rounded border border-yellow-300 text-xl leading-relaxed">
Let's see what Linux gives us today, and why it is not enough.
</div>

</div>

</div>

<!--
So what do people do today? The simplest option is to copy the whole workspace. That is fine for a small repository, but it becomes painful for a ten gigabyte monorepo. Another option is to use git stash between attempts, but git stash only captures tracked files. It misses node_modules, build output, and other ignored state. Some teams use a Docker container for each attempt, but that brings in the Docker daemon, often root, and startup time that is large compared with the work the agent is trying to do.

The last option is to give up on parallelism and retry serially, but that loses the main benefit of exploration. Some teams build a more careful version with chroot, bind mounts, and their own cleanup logic. That is closer to what we want, but it is still racy to set up, and it still does not give us an atomic commit.

What we actually want is one workspace path where the agent runs, plus several copy-on-write branches of that workspace. The first successful branch commits, the losing branches are discarded, and the whole thing should work without root, without a heavy daemon, and across the filesystem you already have. That is the design target for the rest of the talk.
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
$ btrfs subvolume create /repo
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
Let's start with the two filesystem tools people usually reach for first.

OverlayFS gets us part of the way there. You mount it with a lower directory, an upper directory, and a work directory. Reads fall through to the lower directory, while writes go into the upper directory. If we create several of these mounts, we can give each attempt its own view of the workspace.

That gives us a per-branch view, it captures all modifications, and it works over many lower filesystems, so it looks promising at first.

But the details do not line up. The mount command usually needs root. Rootless OverlayFS exists, but in practice it is fragile across distributions. There is also no clean way to commit changes back. If we try to rsync the upper directory into the lower one, we miss the whiteouts that represent deletions, so deleted files can reappear later. OverlayFS also does not give us automatic cleanup for losing branches, and nesting overlays is technically supported but complex and easy to break.

Now compare that with Btrfs and ZFS subvolumes. A Btrfs subvolume snapshot has the right shape in many ways. Creation is O(1), copy-on-write happens at block level, and nested subvolumes are a normal part of the design.

The problem is portability and commit semantics. Btrfs only helps if the workspace is on Btrfs. Your CI may be on ext4, your laptop may be on XFS, and your users may be on something else entirely. There is also no Btrfs operation that means, commit this child back into its base. You are back to copying files yourself. ZFS has a promote operation, but it changes the dataset tree in the opposite direction from what we need, and ZFS also has the well-known mainline licensing problem.

So both mechanisms come close, but both miss the two operations the agent workflow needs most: committing winning changes back, and discarding losing branches. OverlayFS also fails the unprivileged requirement, while Btrfs and ZFS fail the portability requirement.
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
Next, look at the process side. Linux already gives us PID namespaces, mount namespaces, cgroup v2, clone3, and the rest of the namespace toolkit.

Each piece is useful by itself. PID namespaces give us a reliable way to kill everything inside the namespace. cgroup v2 added cgroup.kill in Linux 5.14, which is much cleaner than walking the process tree by hand. clone3 lets us ask for namespaces at process creation time, and mount namespaces give us the private workspace view we need. Traditional process groups, through setpgid and setsid, are useful for cooperative programs, but a child can call setsid and leave the group, so they are not strong isolation.

The important point is that the kernel already has the ingredients. What it does not have is one safe operation that combines them. We need a single operation that says: create the filesystem view, create the process isolation, wire up cleanup, and either finish all of it or cleanly fail.
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
Here is the race in concrete terms.

If we build one branch in userspace, we have to perform a sequence of separate operations. We mount the filesystem branch, unshare the mount namespace, call clone3 with the right flags, move the new process into a cgroup, and then install some kind of signal and ptrace fence between branches. That last fence does not even have a direct kernel primitive today.

The problem is that the child can run between these steps. The common race is between clone3 and adding the child to the cgroup. The parent gets PID 12345 and is about to add it to the cgroup, but before that happens, the child calls fork. Now there is a grandchild, PID 12346, that was never added to the cgroup.

Later, when you try to kill the branch with cgroup.kill, PID 12345 dies, but 12346 survives. At that point you are walking procfs and looking for orphaned processes. That is not isolation; that is cleanup after a race you already lost.

You can work around this by running a PID 1 inside the branch and making it supervise everything, but that is exactly the overhead we were trying to avoid.

The same pattern repeats across the design. Every userspace composition has a window where part of the setup is done and part of it is not. Until the kernel has one operation that does the whole setup together, branch contexts either have to live with races or fall back to heavier isolation.
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
Let me pull the last few slides into one argument, because this is the pivot of the talk.

Every individual primitive does one thing well. OverlayFS gives us per-branch filesystem views. Btrfs gives us cheap snapshots. Cgroups give us reliable group termination. Mount namespaces give us isolated mount tables. clone3 gives us namespace setup at fork time.

Agentic exploration needs all of those pieces at once. When the agent asks for three branches, the OS needs to create the filesystem branches, the mount namespaces, the process groups, the cleanup path, and the child PIDs. The key is that the setup has to be atomic: it should either fully succeed, or fully clean up after itself.

This is not a new kind of problem for Linux. It is the same reason clone exists. Before clone, people could almost build threads from fork, shared memory, and signals, but the result was fragile. clone put that composition inside the kernel. We are making the same argument here: the pieces for fork, branch, isolate, and commit exist, but Linux lacks one kernel operation that combines them safely.

That is the pivot. From here on, I will show the two pieces of our design: BranchFS, which you can install today, and branch(), a kernel syscall that we have as a working prototype.
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
Here is the main abstraction. We call it a branch context. A branch context combines two things: a copy-on-write filesystem view, which I will call the branch delta, and a confined process group. Together, those two pieces form one branch.

The lifecycle has three phases. First, fork: create several branches from a frozen origin. Second, explore: let each branch run independently and build up its own filesystem and process state. Third, commit or abort: the winning branch applies its changes atomically, and the losing branches are discarded. If a branch aborts, it just throws away its own delta.

Four properties define the semantics, and these are what make branch contexts different from a pile of OverlayFS mounts behind a script.

The frozen origin means the base state is read-only while branches exist. There is nothing for the branches to merge against, because the base is not moving. That removes a whole class of conflict handling.

Parallel isolated execution means the branches can run at the same time, but they cannot observe or modify each other's state, even though they started from the same base.

First-commit-wins is our resolution rule. Any branch can commit. The first one to commit wins atomically; all others are discarded. This is the right choice for AI exploration because the orchestrator does not know which path will succeed.

Nestable means a branch can create sub-branches, which forms a tree. Each level commits back to the level above it, which matches Tree-of-Thoughts and similar patterns.

The architecture diagram shows the same idea. The parent process calls branch with N equal to three. That creates three child processes. Each child has its own mount namespace and sees its own BranchFS delta layer, but all three are backed by the same base directory. The kernel syscall coordinates the processes and namespaces, while BranchFS provides the copy-on-write filesystem views.
-->

---

# BranchFS - Speculative Branching Filesystem

<div class="grid grid-cols-2 gap-5 text-sm mt-3">

<div>

### What it does

- **CoW branch** per `@path` over any directory
- First write copies the file into the branch's delta
- **Atomic commit-to-parent**
- **Zero-cost abort** (delete the delta)
- **Nested branches** along a chain back to base
- **No root**, runs on any POSIX FS

</div>

<div>

### Using it

```bash
$ branchfs mount --base /repo /mnt/work
$ branchfs create feature-a /mnt/work
$ cd /mnt/work/@feature-a
$ vim src/parser.py && make test
$ branchfs commit /mnt/work
```

</div>

</div>

<div class="mt-3 p-2 bg-blue-50 rounded border border-blue-300 text-xs text-center">
Every branch is also reachable at <code>/mnt/work/@&lt;name&gt;/</code>. Multiple agents share one mount via <code>@branch</code> paths.
</div>

<div class="mt-3 text-xs text-center opacity-70">
~4,400 LoC Rust &middot; FUSE 3 &middot; MIT/Apache-2.0 &middot; <code>github.com/multikernel/branchfs</code>
</div>

<!--
BranchFS is the userspace half of the design. It implements the filesystem side of a branch context: a copy-on-write branch per @-prefixed path, atomic commit back to the parent branch, and zero-cost abort that just deletes the delta. Branches can nest along a chain back to the base directory.

It is about 4,400 lines of Rust, built on the fuser library, which is the standard Rust binding for the FUSE 3 protocol. It runs entirely as a userspace daemon: no kernel module, no privileged install. That gives us the usual FUSE benefits: anyone can run it, it works across kernels, and bugs crash the daemon rather than the kernel. The performance gap that FUSE used to have is also much smaller now because of FUSE 3 passthrough mode, which I will show in a few slides.

It works over ordinary filesystems: ext4, XFS, Btrfs, tmpfs, NFS, and others. It is open source under MIT and Apache 2.0.

The right column shows the usage. You mount BranchFS over a repository with branchfs mount --base, create a named branch with branchfs create, and get back an @-prefixed path for that branch. Then you cd into that path and work as if you were in a normal repository. The branch can see the base files, but any writes are captured into its own delta layer. When you are done, branchfs commit applies the delta atomically, and branchfs abort throws it away.

The @-path is the key to parallel agents. Every branch is reachable at /mnt/work/@<name>/, and that path resolves to the branch independently of whatever "current branch" the mount happens to be on. So N agents can share one mount and one daemon, each addressing its own @-path, with no per-agent setup and no coordination between them. The demo a few slides from now leans on exactly this.

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
The core mechanism is file-level copy-on-write. The first time a branch writes to a file, BranchFS copies that whole file from the base, or from an ancestor branch, into the branch's delta directory. After that, reads and writes for that file go to the delta copy. Files that the branch never modifies pass through to the base.

This is coarser than block-level copy-on-write in Btrfs. If you touch one byte of a one-megabyte file, Btrfs may copy only a four-kilobyte block, while BranchFS copies the whole megabyte. That is a real cost, and it is the main trade-off.

For agent workloads, the trade-off is usually worth it. The implementation is much simpler: we can use copy_file_range from a FUSE handler, without new kernel code or filesystem-specific metadata. It also keeps BranchFS portable, because we are not tied to Btrfs or any other single filesystem.

The reason this works in practice is that agents usually touch source files, config files, and small build artifacts. Those are usually kilobytes to low megabytes. A one-megabyte copy is around two hundred microseconds, which is still far smaller than the model call that caused the edit. Even a huge node_modules tree only pays this cost on files the branch actually writes.
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
Looking up a file in a branch is a chain walk. When a process opens a path under a branch, BranchFS checks that branch's delta first. If the file is there, we serve it from the delta. If it is not there, we walk back through the branch's ancestors until we reach the base directory, and we serve the first copy we find.

The subtle case is deletion. If you delete a file on a branch and we just remove the delta entry, the next lookup would walk back to the base and find the original copy still there. The file would seem to come back. That's wrong.

The fix is a tombstone. When a branch deletes a file, we write a small marker under .tomb in that branch's delta. The lookup path checks for tombstones at every level, and if it sees one, it returns ENOENT instead of falling through to the base copy.

This is also where the portability story comes from. All BranchFS needs from the underlying filesystem is a writable directory. Creating a branch is mkdir, and destroying a branch is rm -rf. There are no filesystem-specific operations, so ext4 works, NFS works, tmpfs works, and the same daemon works on macOS over APFS. That is the practical answer to "why not just use Btrfs subvolumes": many users are not on Btrfs.
-->


---

# Commit: Apply Winning Changes Atomically

<div class="text-sm mt-3">

A commit applies a winning branch's delta in six steps:

```text
1. Collect modified files + tombstones from Δ
2. Apply tombstones to target Δ        (deletes first)
3. Copy modified files into target Δ   (then creates)
4. Increment target epoch counter      (single atomic op)
5. SIGBUS on losing mmap'd regions     (their state is now stale)
6. Losing branch's next FUSE op returns -ESTALE
```

</div>

<div class="mt-4 p-2 bg-blue-50 rounded border border-blue-300 text-xs">
Order matters: deletes before creates. Otherwise a delete-then-recreate sequence in the branch could miscompose against an unchanged parent file with the same name.
</div>

<!--
A commit applies the winning branch's changes back to the branch it came from. There are six steps, and the order matters.

Step one: collect the modified files and the tombstones from the committing branch's delta.

Step two applies the tombstones to the target delta first. This ordering matters. If the branch deleted a file and then created a new file with the same name, doing the create first would leave the file there and then the tombstone would delete it. By applying tombstones first, we preserve the right semantics.

Step three copies the modified files into the target delta.

Step four: increment the target branch's epoch counter. This is a single atomic operation. It is the moment of commitment. Once this is done, the commit has happened and all losing branches are no longer allowed to continue.

Step five: any losing branch that has memory-mapped a file from this branch sees its mapping go stale. We deliver SIGBUS on next access so the branch notices.

Step six: the next FUSE operation from a losing branch returns -ESTALE. That is the signal to the agent's task wrapper that this branch lost the race and should exit.

The note at the bottom is the rationale for the ordering: deletes before creates avoids a miscomposition bug.
-->

---

# Cost and Performance

<div class="grid grid-cols-2 gap-5 text-sm mt-2">

<div>

### Abort: near-zero cost

- Delete the branch delta; nothing else to do
- No base-copy cleanup needed
- Cost scales with changed files, not workspace size

<div class="mt-4 p-2 bg-blue-50 rounded border border-blue-300 text-xs">
This is BranchFS's <strong>first-commit-wins primitive</strong>. The <code>branch()</code> syscall (later) drives commit/abort through ioctls without changing the semantics.
</div>

</div>

<div>

<div class="font-semibold text-blue-600 mb-1">Branch creation: O(1)</div>

| Base size | Latency |
|:-:|:-:|
| 100 files | 292 μs |
| 1,000 files | 317 μs |
| 10,000 files | 310 μs |

<div class="font-semibold text-green-600 mt-3 mb-1">Commit & Abort: O(delta)</div>

| Mod. size | Commit | Abort |
|:-:|:-:|:-:|
| 1 KB | 317 μs | 315 μs |
| 100 KB | 514 μs | 365 μs |
| 1 MB | 2.1 ms | 890 μs |

</div>

</div>

<div class="mt-3 text-sm text-center">
For agents doing LLM calls of <strong>100 ms – 10 s</strong> per step, <strong>sub-millisecond branching is invisible</strong>.
</div>

<div class="mt-1 text-xs text-center opacity-60">
Hardware: AMD Ryzen 5 5500U (6c/12t), 8 GB DDR4, NVMe SSD. Median of 10 trials.
</div>

<!--
Abort is the cheap side of the branch lifecycle, and the performance numbers tell the story for both directions.

Abort first: rm -rf the branch's delta directory. That's it. Siblings are untouched. The cost is just the unlink work, proportional to whatever the aborted branch had built up. No coordination needed. The epoch counter from the previous slide is what makes the first-commit-wins side cheap too: no global lock, the winner just bumps a counter and siblings notice lazily on their next FUSE op.

Now the numbers. Branch creation is O(1); it doesn't matter whether your base has a hundred files or ten thousand, you pay about 300 microseconds. Because creation is literally a mkdir of the delta directory; no file copying happens until you actually write.

Commit and abort scale with how much you changed, not how big the workspace is. A kilobyte is 317 microseconds, a megabyte is two milliseconds. Abort is even cheaper than commit because it just unlinks.

The framing line is what I want you to walk out with: agents do LLM calls, those calls take 100 ms at the absolute fastest, often several seconds. Sub-millisecond branching is invisible against that. We are not the bottleneck.

The blue callout foreshadows the kernel side: the branch() syscall a few slides from now drives these exact commit and abort operations through ioctls. The semantics don't change. The kernel just adds atomic process coordination on top.

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
- **Opt-in** (`--passthrough`); needs `CAP_SYS_ADMIN`

<div class="mt-3 p-2 bg-blue-50 rounded border border-blue-300 text-xs">
The "FUSE is slow" reputation comes from the default mode's 19% number. Passthrough closes the gap to ~82% with no application changes.
</div>

</div>

</div>

<!--
Here is the FUSE performance story. Native ext4 on this NVMe drive reads at 8.8 gigabytes per second on a 50 megabyte file. BranchFS in default FUSE mode reads at 1.7 gigabytes per second. That is the number people usually point to when they say FUSE is too slow, because the default path sends every read through the daemon.

But: FUSE 3 added passthrough mode in kernel 6.9. Passthrough is exactly what it sounds like, the daemon registers the lower file descriptor with the kernel, and from then on, reads to the upper file go straight to the lower one without round-tripping through the daemon. We use this for all unmodified files. Modified files still go through the daemon because we need to serve from the delta copy.

With passthrough, BranchFS reads at 7.2 gigabytes per second, which is 82 percent of native ext4. That is the number to remember when someone says FUSE is too slow for this workload.

The old reputation is not imaginary; default FUSE mode really is much slower. But passthrough closes most of the gap for unmodified files, and for agent workloads the remaining gap is not the limiting factor.
-->


---

# Demo: A Parallel Agent Run, Start to Finish

<div class="grid grid-cols-2 gap-5 text-sm mt-3">

<div>

### Transcript

```text
$ branchfs mount --base $PWD /mnt/work
$ cd /mnt/work

# Race 3 fixes, first success wins
$ branching speculate \
    -c "./try_fix_a.sh && pytest" \
    -c "./try_fix_b.sh && pytest" \
    -c "./try_fix_c.sh && pytest"

[branching] @fix-b: success, committed
[branching] @fix-a, @fix-c: aborted
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
Now let's walk through what this looks like end to end. This is a real run from my laptop, trimmed down to fit on the slide.

We mount BranchFS over the current directory and cd into the mount. The daemon starts up and reports that FUSE 3 passthrough is available.

Then we drive the parallel run with one command: branching speculate, with three -c commands. That's the user-facing CLI for first-wins speculation. BranchContext is the Python library and CLI we ship on top of BranchFS, and the next slide will go into it. For now, the important thing is that one command takes three shell candidates and races them.

Under the hood, branching speculate creates three named branches via BranchFS, fans out one process per candidate into its @-prefixed path, and waits for any of them to succeed. Each candidate writes to its own delta. The first one whose command exits zero wins.

In this run, fix-b's pytest passed first. BranchContext commits @fix-b atomically: its delta becomes the new base state, and the epoch advances. The other two branches return -ESTALE on their next operation and are aborted automatically.

Notice the disk footprint. Three branches running in parallel did not require three copies of the workspace. Each branch's delta only contains the files it actually modified. If the off-by-one was a one-line change to one Python file, each delta is a few hundred bytes. We just got three-way exploration for the cost of three small files, not three copies of a multi-gigabyte repo.

This is BranchFS plus BranchContext working in userspace today. No kernel changes required. This runs on Ubuntu 22.04, on Fedora, on Arch, on a Mac with macFUSE, wherever you have FUSE 3.
-->

---

# Why a Syscall? Userspace Is Not Enough

<div class="text-sm mt-3">

### What BranchFS gives us today

| Requirement | Status |
|------------|--------|
| R1 isolated views | ✓ via delta layers |
| R2 nesting | ✓ via branch chain |
| R3 complete FS coverage | ✓ via FUSE layer |
| R4 lightweight, unprivileged, portable | ✓ userspace FUSE |
| R5 coordination | **✗, userspace cannot do this safely** |

</div>

<div class="mt-4 p-3 bg-red-50 rounded border border-red-300 text-sm text-center">
Four of five checked. Coordination needs the kernel.
</div>

<!--
At this point, let's pause and check the requirements.

For the first four requirements, BranchFS in userspace gives us what we need. Isolated views come from delta layers. Nesting comes from the branch chain. Complete filesystem coverage comes from intercepting everything at the FUSE layer. Lightweight, unprivileged, and portable operation comes from being a userspace FUSE daemon over an ordinary directory.

The missing requirement is coordination. We cannot get that from a userspace FUSE filesystem alone. We need atomic process spawn into a branch context, reliable termination of all processes in a branch when it commits or aborts, and a fence between sibling branches so they cannot signal each other. We also need all of that to compose atomically with the filesystem branch setup, so there are no race windows. That is the kernel's territory.

So four of the five requirements are handled in userspace, and the last one needs the kernel. Next, I will show what userspace can and cannot do for coordination, and then I will show the kernel race in code.
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
The capabilities we need fall into two groups. Some are simply impossible from userspace. Atomic composition is impossible because userspace cannot turn several kernel operations into one atomic operation. Memory branching, meaning page-table copy-on-write, also has to live in the kernel.

The other capabilities are possible in pieces, but not in the shape we need. Reliable termination can be done with cgroups, but cgroups often need root. Sibling fences can be approximated with PID namespaces, but PID namespaces bring PID 1 overhead. Mount setup is possible with the new mount API, but it is fragile to drive from userspace as a multi-step sequence.

The proposal is branch(): one syscall that composes these pieces atomically and cleans up on partial failure. The next slide shows its interface.
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
| `BR_COMMIT` | child: apply this branch, terminate losing branches |
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
The syscall interface is intentionally small. There is one entry point, branch, and it takes three arguments: an operation code, a pointer to the operation arguments, and the size of that argument block. That size field is the same basic idea used by bpf(2): it lets us extend the structure later without breaking the ABI.

There are three operations. BR_CREATE is called by the parent and creates N branches while forking N children. BR_COMMIT is called by a child when that child wants to commit its branch. If it wins, the losing branches are terminated. BR_ABORT is also called by a child, and it simply discards that child's branch.

BR_CREATE has four flags that say which resources should be branched. BR_FS is required, because it gives us the mount namespace and the BranchFS branch. BR_MEMORY adds page-table copy-on-write for memory; I will come back to that as future work. BR_ISOLATE adds a kernel-enforced signal and ptrace fence between sibling branches, so they cannot signal or trace each other even as the same user. BR_CLOSE_FDS closes inherited file descriptors, so children re-open files inside their own branch context.

That is the whole surface area: three operations and four flags. Now let me show what using it looks like.
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
Here is what calling branch() looks like end to end. The parent opens the workspace as an O_PATH file descriptor. It fills in the create arguments with the BR_FS flag, the mount file descriptor, the branch count, and an output buffer for the child PIDs. Then it calls branch(BR_CREATE).

The same syscall returns in two different roles. In the parent, it returns zero and fills in the PID array. In each child, it returns one, two, or three, which is that child's branch index. So one syscall gives us different processes, different mount namespaces, and different BranchFS deltas, without a userspace setup race.

Each child then tries its fix. If the fix passes, the child calls BR_COMMIT. The kernel resolves the race with an atomic compare-and-swap on the winner field. If this child won, the syscall returns zero and its changes are committed. If another child committed first, the syscall returns -ESTALE, and this child exits. If the fix fails, the child calls BR_ABORT, and the branch is discarded.

The key property is that the commit race is resolved inside the kernel. The losing children get a clear -ESTALE return code, and userspace does not need to coordinate the race itself.
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
Before we look at the kernel internals, there is one design choice that matters for the wider ecosystem. The branch() syscall does not do filesystem-specific work. Branch lookup, delta management, and commit logic stay in the branching filesystem. The syscall only talks to that filesystem through three generic ioctls: create, commit, and abort.

This follows a pattern Linux already uses. FICLONE, FIEMAP, and FIDEDUPERANGE are generic operations that different filesystems can implement. They are not tied to one filesystem. We use the same idea here: any filesystem that wants to support branching can implement the three ioctls.

That gives us a clean extension point. BranchFS implements these ioctls today, so the syscall works with BranchFS out of the box. In the future, Btrfs could implement the same contract with real subvolumes, or OverlayFS could add a commit mode. Adding a new backend would not require a new syscall or a new VFS interface.

In the prototype, BR_CREATE works like this. Userspace calls branch with a branch count and a mount file descriptor. The kernel sends FS_IOC_BRANCH_CREATE to the mounted filesystem once per branch. Each request crosses into the BranchFS daemon, which allocates a delta directory and returns a branch name. Then the kernel forks the child processes. For each child, the kernel sets the return value so the child sees its branch index instead of zero. That gives us the single-syscall behavior that the userspace API expects.
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
The prototype is a small patch series against vanilla Linux 6.17. The first patch adds the UAPI and internal kernel headers. That patch is types only. The second patch implements the syscall body and connects the ioctl path to BranchFS. The third patch adds the hooks in copy_process and do_exit, plus the new task_struct fields those hooks need.

The build is meant to be easy to reproduce. On a 24-core machine, the full build and test run takes about five minutes. The script clones Linux 6.17, applies the patches, and builds the kernel. Cargo builds BranchFS. Make builds the C test program. Another script builds the initramfs, and run-qemu boots the patched kernel under QEMU with KVM, runs the tests, and powers off.

The status table is honest about what works and what does not. The three core operations, create, commit, and abort, work. First-commit-wins works. Sibling SIGKILL works. The bridge to BranchFS through the FS_IOC_BRANCH_* ioctls works. The BR_ISOLATE flag is accepted, but the kernel-side fence is not wired up yet. Mount-namespace setup with bind mounts is also deferred; for now, children chdir into their @-branch directories. BR_MEMORY returns -EOPNOTSUPP, and nested branches return -EBUSY. Those are the next pieces of work.

The QEMU test harness covers single commit, a three-way commit race, sibling SIGKILL, abort, and a latency micro-benchmark. It prints rc equals zero at the end, with no oopses and no warnings. This is prototype code, but it is real code, not slideware.
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
The latency test gives us the numbers. In steady state, after the cold-cache first iteration, BR_CREATE takes 61 to 70 microseconds on the parent side. That is the full round trip: one vfs_ioctl to BranchFS through FUSE, one kernel_clone, the branch hook, the return-value setup for each child, and the copy_to_user of the child PIDs back to the parent.

BR_COMMIT on the child side takes 12 to 25 microseconds in steady state. That includes the atomic winner check, the vfs_ioctl to BranchFS for the commit, and sibling cleanup. For this small benchmark, sibling cleanup is basically free.

The paper reports BranchFS branch creation at about 300 microseconds. The syscall path is faster because the paper measures the branchfs command-line path, including process startup, argument parsing, and the daemon control socket. The kernel path bypasses that and talks directly to the FUSE daemon through the ioctl.

The cost breakdown for BR_CREATE is also useful. The vfs_ioctl path to BranchFS is the largest single chunk, about 28 microseconds, dominated by the FUSE protocol round trip and the kernel-to-userspace context switch. kernel_clone is about 25 microseconds. Our own hook is about 12 microseconds. The remaining bookkeeping is about five microseconds. None of those costs are anywhere near the size of a Python import, let alone an LLM call.

I want to be honest about these numbers. They are sanity-check numbers from a 4-vCPU QEMU guest. They tell you the prototype is not doing anything pathological. Paper-quality numbers would need bare metal, multiple base sizes, p99 distributions, and a head-to-head comparison against the closest userspace equivalent, something like unshare plus OverlayFS plus a shell script. That work is on the to-do list and would be a nice OSSummit talk in itself.

The most important number is the ratio. A model or tool step is usually between 100 milliseconds and 10 seconds. BR_CREATE is around 70 microseconds. That gives us at least three orders of magnitude of headroom, so branching is in the noise for agent workloads.
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
Most people will not call the syscall directly. They will use a library. We ship one for Python, because that is where most agent code lives today.

BranchContext wraps the BranchFS primitives into exploration patterns. Speculate races several candidates and commits the first success. BestOfN runs all candidates and commits the highest-scoring result. Reflexion does retry with feedback from the previous failure. TreeOfThoughts creates nested branches. BeamSearch keeps the top K candidates at each level. Tournament does pairwise elimination with a judge function. Cascaded starts with one branch and fans out only when it needs to.

Each pattern is a small amount of Python around the BranchFS primitives. The library handles branch creation, parallel execution, success or failure judging, and cleanup.

The agent author opens a BranchContext over the BranchFS mount. Then they call best_of_n with N equal to three, a per-branch task function, and a scoring function. That is it. The library creates the branches, runs the tasks in parallel, scores each one, commits the best, and discards the rest. The agent author writes lambdas, not subprocess plumbing.

The migration story is deliberate. Today, BranchContext calls BranchFS via its CLI and ioctls. When the branch() syscall is upstream, the library can switch to using the syscall, with the same Python API and more atomic behavior underneath. We want agent authors to write to a stable API now and benefit from kernel improvements later without rewriting their code.

Today this is pure userspace. You pip install branchcontext, mount BranchFS, and use the same Python API. It works on Linux, and it also works on macOS through macFUSE.
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
Here is where the project stands today. BranchFS is production-quality enough that people are using it on Linux laptops, in CI, and on macOS with macFUSE. BranchContext is on PyPI, so pip install gives you the library. The branch() prototype is in the repo with the three patches, the QEMU test harness, and a passing test run.

There are also limitations I want to call out before someone asks.

External side effects are not rolled back on abort. That means network calls, IPC, or anything else that escapes the filesystem. If a branch sends an email, that email is already gone. We do not have external effect control yet, and that is a research direction.

Single-winner only. There's no multi-branch merge. If you wanted to combine non-overlapping changes from two branches, you'd have to do it yourself outside the framework.

File-level copy-on-write also has partial support for trickier file types. Absolute symlinks, hardlinks, FIFOs, sockets, and device nodes all need more work for general use. They are enough for the agent workloads we target today, but they are not the full story.

BR_MEMORY is deferred. Page-table copy-on-write is real memory-management work. It is not conceptually hard, but it needs careful code, so we designed for it but did not implement it yet.

Nested branches are also deferred in the kernel prototype, although the BranchFS branch chain already handles them. On the kernel side, the remaining work is mostly removing one guard and handling the commit rule for nested branches.
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

**External effect control**: hold network / IPC until commit.
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
Here is the roadmap for the next stage. The first step is to port the prototype forward to current mainline, prepare an RFC patch series, and send it to linux-kernel. The two hooks are small enough that we think this is a realistic upstream conversation. If you are a kernel reviewer and want to look before that lands, please come find us afterward.

The next step is to finish BR_ISOLATE. That is the signal and ptrace fence that makes sibling isolation safe even when a branch is buggy or hostile, not only when it is polite. After that, we want nested branches in the kernel. BranchFS already handles arbitrary depth in its branch chain; the kernel side needs to remove one guard and handle the commit rule for a branch inside another branch.

The bigger research direction is external effect control. Filesystem effects are easy to roll back, but network calls and IPC are not. The long-term direction is to hold those effects until commit and discard them on abort. Agent gateways are a natural place to do that because they already sit between the agent and the outside world.

There is also a broader use case beyond agents. If n_branches is one, branch() becomes a general try-and-rollback primitive. You can imagine package upgrades, system configuration changes, or schema migrations using the same lifecycle: try the change, commit if it works, and roll back if it fails. Agents are the loudest current use case, but the abstraction is more general.
-->


---

# How to Try It, How to Help

<div class="grid grid-cols-2 gap-4 text-xs mt-1">

<div>

### Try it

```bash
# BranchFS - works on any Linux today
$ cargo install branchfs
$ branchfs mount --base /repo /mnt/work
$ pip install branchcontext
```

(Optional) Kernel patches: <https://github.com/yunwei37/agentfs/tree/main/prototype/patches>

### Help wanted

- **Bugs / features** → GitHub issues
- **Kernel review** → LKML thread (soon)
- **New FS backends** → implement `FS_IOC_BRANCH_*`
- **Agent integrations** → BranchContext patterns

</div>

<div>

### Resources

- BranchFS - `github.com/multikernel/branchfs`
- BranchContext - `github.com/multikernel/branching`
- Original Paper - <https://arxiv.org/abs/2602.08199>

</div>

</div>

<!--
If you want to try it, the userspace path is short. cargo install branchfs gives you the daemon, and pip install branchcontext gives you the Python library. Mount BranchFS over a repository, point BranchContext at the mount, and you can start using the exploration patterns without a patched kernel.

If you want to try the kernel prototype, clone the repo and run the scripts. They build the patched kernel, build the test program, create the initramfs, and boot QEMU. On a reasonably fast machine, you can have the full test run in a few minutes.

We are looking for help in four areas. Bugs and feature requests should go to GitHub. Kernel review will matter once the patches are forward-ported and sent to LKML. New filesystem backends can implement the three FS_IOC_BRANCH_* ioctls. Agent integrations can add new BranchContext patterns or wrappers for other languages.

The repos are listed here: BranchFS, BranchContext, and the paper-plus-prototype repository. The userspace code is dual MIT and Apache-2.0, and the kernel patches are GPL-2.0. Please reach out if you want to use this, review it, or tell us where the design does not fit your workload.
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
Let me close with the main takeaways. AI agents look like ordinary Linux processes, but they create side effects in a pattern that existing tools do not handle well. The kernel sees shell commands, file writes, package installs, and test runs. It does not know that one process is only one speculative path among many.

The hard part is composition. OverlayFS gives isolation but not clean commit. Btrfs gives snapshots but not portability. Namespaces and cgroups give process isolation, but they bring privilege requirements and race windows when userspace tries to stitch them together. The missing piece is one lifecycle that joins filesystem state and process state.

Our proposed abstraction is the branch context: a copy-on-write filesystem view plus a confined process group. The lifecycle is fork, explore, and commit. The first winner lands, the losing branches disappear, and nesting is part of the model.

BranchFS is the userspace half, and it works today. It is FUSE 3, Rust, no root, and portable across normal filesystems. The performance is fine for agent workloads, and FUSE passthrough closes most of the read-throughput gap. The branch() syscall is the kernel path. The prototype is small, it passes tests in QEMU, and an RFC patch series is the next step.

If you have been trying to run agents and found yourself reaching for cp -r or Docker just to isolate attempts, please take a look at the repos. I would also be happy to talk afterward about where this design fits, and where it does not.
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
Thank you. I am happy to take questions.

Common questions are why we used FUSE instead of a kernel filesystem, how this compares to running each attempt in its own container, and what the path to mainline might look like for branch(). I am also happy to talk about non-agent use cases, like package management, schema migrations, and other places where a lightweight try-and-rollback primitive would be useful.
-->
