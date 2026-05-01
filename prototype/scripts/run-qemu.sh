#!/usr/bin/env bash
# run-qemu.sh — boot the patched kernel + initramfs and run test_branch.
#
# Usage:
#   ./run-qemu.sh [BZIMAGE] [INITRAMFS]
#
# Defaults assume scripts/build-kernel.sh and build-rootfs.sh have been run.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTO_DIR="$(cd "$HERE/.." && pwd)"
BZIMAGE="${1:-$PROTO_DIR/../linux-vanilla/arch/x86/boot/bzImage}"
INITRAMFS="${2:-$PROTO_DIR/build/initramfs.cpio.gz}"

[[ -f $BZIMAGE   ]] || { echo "missing $BZIMAGE; run build-kernel.sh first";   exit 1; }
[[ -f $INITRAMFS ]] || { echo "missing $INITRAMFS; run build-rootfs.sh first"; exit 1; }

KVM=()
[[ -r /dev/kvm && -w /dev/kvm ]] && KVM=(-enable-kvm -cpu host)

exec qemu-system-x86_64 \
    "${KVM[@]}" \
    -smp 4 -m 2G \
    -kernel "$BZIMAGE" \
    -initrd "$INITRAMFS" \
    -append "console=ttyS0 nokaslr branchtest_auto" \
    -nographic -serial mon:stdio -no-reboot
