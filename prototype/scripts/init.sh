#!/bin/sh
# Init script for branch() syscall + BranchFS test environment.

mount -t proc  none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev 2>/dev/null || mount -t tmpfs none /dev
mkdir -p /dev/pts
mount -t devpts none /dev/pts 2>/dev/null

hostname branchtest

cat <<EOF
==========================================================
   branch() + BranchFS QEMU test environment
   kernel: $(uname -r)
==========================================================
EOF

# /dev/fuse should already exist via devtmpfs
ls -l /dev/fuse 2>/dev/null || mknod /dev/fuse c 10 229
chmod 666 /dev/fuse

mkdir -p /base /work /var/lib/branchfs

echo "[init] starting branchfs daemon (base=/base, mountpoint=/work)..."
/bin/branchfs mount --base /base --storage /var/lib/branchfs /work &
BFS_PID=$!

# Wait up to 5s for the mount to come up.
for i in 1 2 3 4 5 6 7 8 9 10; do
    if [ -e /work/.branchfs_ctl ]; then
        echo "[init] /work/.branchfs_ctl appeared after ${i}00ms"
        break
    fi
    sleep 0.1 2>/dev/null || /bin/sleep 1
done

if [ ! -e /work/.branchfs_ctl ]; then
    echo "[init] BranchFS failed to come up, dropping to shell"
    exec /bin/sh
fi

echo "[init] BranchFS ready. Running test_branch..."
echo "----------------------------------------------------------"

# Run as root in initramfs.
/bin/test_branch all 50
TRC=$?

echo "----------------------------------------------------------"
echo "[init] test_branch exited with rc=$TRC"

# Drop to shell unless 'auto' on cmdline (then poweroff for clean exit)
if grep -q branchtest_auto /proc/cmdline 2>/dev/null; then
    sync
    poweroff -f
else
    exec /bin/sh
fi
