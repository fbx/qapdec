#!/bin/sh

SCRIPT_DIR=$(cd `dirname $0` && echo $PWD)

if [ "$#" -ne 1 -o ! -d "$1" ]; then
  echo "usage: $0 <APQ8098.LE.1.0_RELEASE_DIR>"
  exit 1
fi

QCA="$1"

if [ ! -f "$QCA/contents.xml" ]; then
  echo "$QCA/contents.xml not found"
  exit 1
fi

export PATH="$QCA/apps_proc/poky/build/tmp-glibc/sysroots/x86_64-linux/usr/bin/aarch64-oe-linux:$PATH"
export CROSS="aarch64-oe-linux-"
export SYSROOT="$QCA/apps_proc/poky/build/tmp-glibc/sysroots/apq8098"

exec $SCRIPT_DIR/build-static.sh $@
