#!/bin/sh -e

SCRIPT_DIR=$(cd `dirname $0` && echo $PWD)

if [ "$#" -ne 2 -o ! -d "$1" ]; then
  echo "usage: $0 <BUILDROOT> <name>"
  exit 1
fi

QCA="$1"
CHROOT="qapdec-chroot-`date +%Y%m%d`-$2"

if [ ! -f "$QCA/buildroot/Makefile" ]; then
  echo "$QCA is not a freebox buildroot"
  exit 1
fi

export CROSS=$(sed -n "s/^CONFIG_TARGET_CROSS=\"\(.*\)\"$/\1/p" "$QCA/buildroot/.config")
export BUILDROOT="$QCA/buildroot/build"

"$SCRIPT_DIR/build-static.sh" "$@"

rm -rf "$CHROOT"
mkdir -p "$CHROOT/usr/bin" \
	"$CHROOT/usr/lib" \
	"$CHROOT/lib"
ln -s lib "$CHROOT/lib64"
ln -s lib "$CHROOT/usr/lib64"
cp qapdec qaptest "$CHROOT/usr/bin/"
cp "$BUILDROOT/usr/lib/libdolby_ms12_wrapper.so" "$CHROOT/usr/lib/"
cp "$BUILDROOT/usr/lib/liblmclient.so" "$CHROOT/usr/lib/"

libpath=$("${CROSS}gcc" $CFLAGS -print-search-dirs | grep '^libraries' | cut -f2 -d = )

"$QCA/buildroot/scripts/reducelibs.pl" --no-reduction \
	-d "$CHROOT/lib" \
	--target "$CROSS" \
	-L "$libpath:$BUILDROOT/lib:$BUILDROOT/usr/lib" \
	-v -P "$QCA/buildroot/hostbuild/usr/bin/probeelf" \
	"$CHROOT"

find "$CHROOT/lib" "$CHROOT/usr/lib" -type f | while read f; do
    case "$f" in
	*libdolby_ms12_wrapper.so)
	    ;;
	*)
	    "${CROSS}strip" "$f"
	    ;;
    esac
done

cp qapdec-chroot.sh "$CHROOT/"
mkdir -p "$CHROOT/rodata"
cp /media/hd/ac3/10frames.ac3 "$CHROOT/rodata/"
cp /media/hd/aac/short.aac "$CHROOT/rodata/"
tar czf "$CHROOT.tar.gz" "$CHROOT/"
