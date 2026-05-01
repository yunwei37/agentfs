# test_branch: userspace driver for the branch() syscall on a BranchFS mount.
#
# Statically linked so it runs in a minimal initramfs without libc.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wno-format -static

test_branch: test_branch.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: clean
clean:
	rm -f test_branch
