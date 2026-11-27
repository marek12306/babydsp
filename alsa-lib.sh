#!/bin/sh
# Cross-builds alsa-lib 1.0.18, the version and configuration of SqueezeOS
# 7.x, into a sysroot that provides the headers and libasound.so the plugin
# is compiled and linked against. Nothing from it is installed on the radio.
#
# Usage: ./alsa-lib.sh [CROSS_COMPILE] [SYSROOT]
#   defaults: arm-linux- sysroot
set -eu

CROSS=${1:-arm-linux-}
SYSROOT=$(realpath -m "${2:-sysroot}")
VER=1.0.18
TAR=alsa-lib-$VER.tar.bz2
SHA256=c487645d32b8124b9ae224b8929e5aa45e581bd8d8c8da666add423cf927d1ea
WORK=$(realpath -m build-alsa-lib)

mkdir -p "$WORK"
cd "$WORK"
if [ ! -f "$TAR" ]; then
	curl -sSfL -o "$TAR" "https://www.alsa-project.org/files/pub/lib/$TAR"
fi
echo "$SHA256  $TAR" | sha256sum -c -
rm -rf "alsa-lib-$VER"
tar -xjf "$TAR"
cd "alsa-lib-$VER"

# Options of SqueezeOS meta-squeezeos/packages/alsa/alsa-lib_1.0.18.bb
./configure --host="${CROSS%-}" CC="${CROSS}gcc" --prefix="$SYSROOT" \
	--disable-aload --enable-mixer --enable-pcm --disable-rawmidi \
	--disable-hwdep --disable-seq --disable-instr --disable-alisp \
	--enable-old-symbols --disable-python \
	--with-pcm-plugins=hw,plug,extplug,multi,rate,route,empty >/dev/null
make -j"$(nproc)" -C include >/dev/null
make -j"$(nproc)" -C src >/dev/null
make -C src install >/dev/null
# All headers, as SqueezeOS's do_stage: "make install" leaves out the
# pcm_extplug.h/pcm_ioplug.h that pcm_external.h includes.
mkdir -p "$SYSROOT/include/alsa"
cp include/*.h "$SYSROOT/include/alsa/"
ls -l "$SYSROOT/lib/libasound.so"
