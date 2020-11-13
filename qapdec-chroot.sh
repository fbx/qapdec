#!/bin/sh -e

SCRIPTDIR=$(cd `dirname $0` && echo $PWD)

ROOT="$SCRIPTDIR"

if [ ! -f "$ROOT/usr/bin/qapdec" ]; then
    echo "error: $ROOT is not a qapdec chroot" >&2
    exit 1
fi

cleanup() {
    echo "Cleanup $ROOT"
    umount -f "$ROOT/bin"
    umount -f "$ROOT/dev"
    umount -f "$ROOT/data"
    umount -f "$ROOT/firmware"
    umount -f "$ROOT/proc"
    umount -f "$ROOT/tmp"
}

trap cleanup INT EXIT QUIT

mount -o remount,exec /data

umask 022
chown root:root -R "$ROOT"

mkdir -p "$ROOT/data"
mount --bind /data/ "$ROOT/data/"

mkdir -p "$ROOT/dev"
mount --bind /dev/ "$ROOT/dev/"

mkdir -p "$ROOT/firmware"
mount --bind /firmware/ "$ROOT/firmware/"

mkdir -p "$ROOT/proc"
mount --bind /proc/ "$ROOT/proc"

mkdir -p "$ROOT/tmp"
mount -t tmpfs none "$ROOT/tmp/"

mkdir -p "$ROOT/bin"
mount -t tmpfs none "$ROOT/bin/"
cp /bin/busybox "$ROOT/bin/"
ln -snf busybox "$ROOT/bin/cat"
ln -snf busybox "$ROOT/bin/cd"
ln -snf busybox "$ROOT/bin/chmod"
ln -snf busybox "$ROOT/bin/chown"
ln -snf busybox "$ROOT/bin/cmp"
ln -snf busybox "$ROOT/bin/cp"
ln -snf busybox "$ROOT/bin/echo"
ln -snf busybox "$ROOT/bin/false"
ln -snf busybox "$ROOT/bin/ls"
ln -snf busybox "$ROOT/bin/ln"
ln -snf busybox "$ROOT/bin/mkdir"
ln -snf busybox "$ROOT/bin/mv"
ln -snf busybox "$ROOT/bin/pwd"
ln -snf busybox "$ROOT/bin/rm"
ln -snf busybox "$ROOT/bin/sh"
ln -snf busybox "$ROOT/bin/true"

chroot "$ROOT" "$@"
