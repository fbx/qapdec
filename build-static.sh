#!/bin/sh -e

SCRIPT_DIR=$(cd `dirname $0` && echo $PWD)

CC="${CROSS}gcc"
CFLAGS="-O2"

TRIPLE=$(${CROSS}gcc -dumpmachine)
ARCH=$(echo $TRIPLE | cut -d- -f1)

CPU_CORES=$(grep -c '^processor' /proc/cpuinfo)
MAKE_JOBS="$CPU_CORES"

die() {
    echo "error: $*" >&2
    exit 1
}

p() {
    echo
    echo "[1;32m> [1;37m$*[0m"
    echo
}

p2() {
    echo "[1;34m * [1;37m$*[0m"
}

if [ ! -d "$SYSROOT" -a ! -d "$BUILDROOT" ]; then
    die "You must set the SYSROOT environment variable to point to an APQ8098 LE sysroot directory"
fi

D="$SCRIPT_DIR/build"

if [ -n "$CLEAN" ]; then
    p2 clean
    rm -rf "$D/src" "$D/build"
fi

mkdir -p "$D/dl"
mkdir -p "$D/src"
mkdir -p "$D/build"
mkdir -p "$D/staging"

CFLAGS="$CFLAGS -isystem $D/staging/usr/include"

if [ -n "$SYSROOT" ]; then
    CC="$CC --sysroot $SYSROOT"
elif [ -n "$BUILDROOT" ]; then
    CFLAGS="$CFLAGS -isystem $BUILDROOT/usr/include"
    LDFLAGS="$LDFLAGS -Wl,-Y,$BUILDROOT/usr/lib64"
    LDFLAGS="$LDFLAGS -Wl,-rpath-link,$BUILDROOT/usr/lib64"
    SYSROOT="$BUILDROOT"
fi

export PATH="$D/host/bin:$PATH"
export PKG_CONFIG_SYSROOT="$SYSROOT"
export PKG_CONFIG_LIBDIR="$D/staging/usr/lib/pkgconfig:$D/staging/usr/share/pkgconfig:$SYSROOT/usr/lib64/pkgconfig"

p "FFTW3"

cd "$D"

pkg="fftw-3.3.8"
archive="$pkg.tar.gz"
S="$D/src/$pkg"

if [ ! -d "$S" ]; then
    p2 "download $pkg"
    wget -c -nv -P dl "http://fftw.org/$archive"
    tar xf "dl/$archive" -C src
fi

if [ ! -e "$S/configure" ]; then
    p2 "autoconf $pkg"
    (cd "$S" && autoreconf -fi)
fi

mkdir -p build/target/fftw
cd build/target/fftw

if [ ! -e Makefile ]; then
    p2 "configure $pkg"
    "$S/configure" --silent --prefix="$D/staging/usr" \
        --host="$TRIPLE" \
        --target="$TRIPLE" \
        --enable-silent-rules \
        --disable-shared \
        --enable-static \
        --disable-debug \
        --disable-doc \
        --enable-neon \
        --disable-fortran \
        --disable-openmp \
        --disable-threads \
        CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" \
        PKG_CONFIG_LIBDIR="$D/staging/usr/lib/pkgconfig"
fi

p2 "build $pkg"

touch .stamp-build
make -s -j"$MAKE_JOBS"

if [ -n "`find -H -type f -newer .stamp-build`" ]; then
    p2 "install $pkg"
    make -s install >/dev/null
fi

p "FFMPEG"

cd "$D"

pkg="ffmpeg-4.3.1"
archive="$pkg.tar.bz2"
S="$D/src/$pkg"

if [ ! -d "$S" ]; then
    p2 "download $pkg"
    wget -c -nv -P dl "http://ffmpeg.org/releases/$archive"
    tar xf "dl/$archive" -C src
fi

mkdir -p build/target/ffmpeg
cd build/target/ffmpeg

if [ ! -e Makefile ]; then
    p2 "configure $pkg"
    "$S/configure" --prefix="$D/staging/usr" \
        --enable-cross-compile \
        --enable-static \
        --disable-shared \
        --arch="$ARCH" \
        --target-os=linux \
        --cross-prefix="$CROSS" \
        --cc="$CC" \
        --extra-cflags="$CFLAGS" \
        --extra-ldflags="$LDFLAGS" \
        --pkg-config=pkg-config \
        --disable-doc \
        --disable-htmlpages \
        --disable-manpages \
        --disable-podpages \
        --disable-txtpages \
        --disable-debug \
        --disable-nonfree \
        --disable-gpl \
        --disable-iconv \
        --disable-autodetect \
        --disable-programs \
        --disable-swscale \
        --enable-swresample \
        --enable-avdevice \
        --enable-avfilter \
        --enable-small \
        --disable-everything \
        --enable-indev=lavfi \
        --enable-decoder=aac \
        --enable-decoder=aac_latm \
        --enable-decoder=ac3 \
        --enable-decoder=eac3 \
        --enable-decoder=dca \
        --enable-filter=sine \
        --enable-muxer=adts \
        --enable-muxer=latm \
        --enable-demuxer=aac \
        --enable-demuxer=loas \
        --enable-demuxer=ac3 \
        --enable-demuxer=eac3 \
        --enable-demuxer=truehd \
        --enable-demuxer=dts \
        --enable-demuxer=dtshd \
        --enable-demuxer=avi \
        --enable-demuxer=matroska \
        --enable-demuxer=mov \
        --enable-demuxer=mpegps \
        --enable-demuxer=mpegts \
        --enable-demuxer=wav \
        --enable-demuxer=hls \
        --enable-bsf=dca_core \
        --enable-parser=aac \
        --enable-parser=aac_latm \
        --enable-parser=ac3 \
        --enable-parser=dca \
        --enable-network \
        --enable-protocol=file \
        --enable-protocol=hls \
        --enable-protocol=http
fi

p2 "build $pkg"
touch .stamp-build
V=1 make -j"$MAKE_JOBS"

if [ -n "`find -H -type f -newer .stamp-build`" ]; then
    p2 "install $pkg"
    make -s install >/dev/null
fi

p "QAPDEC"

cd "$SCRIPT_DIR"

make -j"$MAKE_JOBS" CROSS="$CROSS" CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS"

# remove this if debugging is needed
${CROSS}strip qapdec

p "Build done. You can copy qapdec to your rootfs"
