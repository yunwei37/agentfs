#!/usr/bin/env bash
# build-kernel.sh — clone vanilla Linux v6.17, apply branch() patches, build.
#
# Output: $LINUX_DIR/arch/x86/boot/bzImage
#
# Usage:
#   ./build-kernel.sh [LINUX_DIR]
#
# LINUX_DIR defaults to ../linux-vanilla relative to this script's dir.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTO_DIR="$(cd "$HERE/.." && pwd)"
LINUX_DIR="${1:-$PROTO_DIR/../linux-vanilla}"
PATCH_DIR="$PROTO_DIR/patches"

echo "[+] building kernel into $LINUX_DIR"
echo "[+] applying patches from $PATCH_DIR"

if [[ ! -d "$LINUX_DIR" ]]; then
    echo "[+] clone vanilla v6.17 (shallow)"
    git clone --branch v6.17 --depth 1 https://github.com/torvalds/linux "$LINUX_DIR"
fi

cd "$LINUX_DIR"

# Idempotent apply: skip patches that look already applied.
if [[ ! -f kernel/branch.c ]]; then
    echo "[+] applying patches"
    git -c user.email=local@local -c user.name=local am "$PATCH_DIR"/*.patch
else
    echo "[=] patches appear already applied (kernel/branch.c exists), skipping"
fi

echo "[+] configuring"
make x86_64_defconfig >/dev/null

cat >> .config <<'EOF'
CONFIG_BRANCH=y
CONFIG_FUSE_FS=y
CONFIG_VIRTIO=y
CONFIG_VIRTIO_PCI=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_NET=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y
EOF
yes "" | make olddefconfig >/dev/null

echo "[+] building (-j$(nproc))"
make -j"$(nproc)"

echo
echo "[OK] kernel ready: $LINUX_DIR/arch/x86/boot/bzImage"
