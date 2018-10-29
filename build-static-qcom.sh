#!/bin/sh -e

SCRIPT_DIR=$(cd `dirname $0` && echo $PWD)

if [ "$#" -ne 2 -o ! -d "$1" ]; then
  echo "usage: $0 <APQ8098.LE.1.0_RELEASE_DIR> <name>"
  exit 1
fi

QCA="$1"
NAME="qapdec-`date +%Y%m%d`-$2"

if [ ! -f "$QCA/contents.xml" ]; then
  echo "$QCA/contents.xml not found"
  exit 1
fi

export PATH="$QCA/apps_proc/poky/build/tmp-glibc/sysroots/x86_64-linux/usr/bin/aarch64-oe-linux:$PATH"
export CROSS="aarch64-oe-linux-"
export SYSROOT="$QCA/apps_proc/poky/build/tmp-glibc/sysroots/apq8098"

$SCRIPT_DIR/build-static.sh $@

git archive --prefix="$NAME/" HEAD | tar x
cp qapdec "$NAME/"
zip -r "$NAME.zip" "$NAME"
