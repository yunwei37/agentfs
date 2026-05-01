#!/usr/bin/env bash
# build-rootfs.sh — assemble an initramfs that boots into BranchFS + tests.
#
# Layout produced under $OUT_DIR:
#   rootfs/                  CPIO source tree
#   initramfs.cpio.gz        the gzip'd CPIO archive
#
# Inputs:
#   $BRANCHFS_BIN  -- path to the BranchFS release binary (Rust)
#   $TEST_BIN      -- path to the static test_branch binary (built from
#                     prototype/test)
#
# Usage:
#   BRANCHFS_BIN=/path/to/branchfs TEST_BIN=/path/to/test_branch \
#       ./build-rootfs.sh [OUT_DIR]
#
# OUT_DIR defaults to ../build under prototype/.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTO_DIR="$(cd "$HERE/.." && pwd)"
OUT_DIR="${1:-$PROTO_DIR/build}"

: "${BRANCHFS_BIN:?need BRANCHFS_BIN=/path/to/branchfs (build branchfs first)}"
: "${TEST_BIN:?need TEST_BIN=/path/to/test_branch (run make in prototype/test/)}"

ROOTFS="$OUT_DIR/rootfs"
mkdir -p "$ROOTFS"/{bin,sbin,etc,proc,sys,dev,tmp,base,work,var/lib/branchfs,lib,lib64,usr/bin,usr/lib/x86_64-linux-gnu}

# busybox: prefer system-static; otherwise fall back to /usr/bin/busybox.
BUSYBOX="$(command -v busybox)"
if ! file "$BUSYBOX" | grep -q "statically linked"; then
    echo "[!] system busybox is not static; install busybox-static"
fi
cp "$BUSYBOX" "$ROOTFS/bin/busybox"

cd "$ROOTFS/bin"
for cmd in sh ash ls cat echo mount umount mkdir mknod rm cp mv grep sed ps top \
           kill sleep date dmesg hostname head tail wc tr cut sync poweroff stat env touch; do
    [[ -e $cmd ]] || ln -s busybox $cmd
done
cd - >/dev/null

# branchfs daemon and its userspace deps
cp "$BRANCHFS_BIN" "$ROOTFS/bin/branchfs"
cp /usr/bin/fusermount "$ROOTFS/usr/bin/fusermount"
cp /usr/lib/x86_64-linux-gnu/libfuse.so.2.9.9 "$ROOTFS/usr/lib/x86_64-linux-gnu/"
ln -sf libfuse.so.2.9.9 "$ROOTFS/usr/lib/x86_64-linux-gnu/libfuse.so.2"
for lib in libgcc_s.so.1 libc.so.6 libdl.so.2 libpthread.so.0 libm.so.6; do
    [[ -e /lib/x86_64-linux-gnu/$lib ]] && cp "/lib/x86_64-linux-gnu/$lib" "$ROOTFS/lib/x86_64-linux-gnu/"
done
cp /lib64/ld-linux-x86-64.so.2 "$ROOTFS/lib64/"

# test binary
cp "$TEST_BIN" "$ROOTFS/bin/test_branch"

# init script
install -m 0755 "$PROTO_DIR/scripts/init.sh" "$ROOTFS/init"

cd "$ROOTFS" && find . | cpio -o -H newc 2>/dev/null | gzip > "$OUT_DIR/initramfs.cpio.gz"
cd - >/dev/null

echo "[OK] $(du -sh $OUT_DIR/initramfs.cpio.gz | cut -f1) initramfs at $OUT_DIR/initramfs.cpio.gz"
