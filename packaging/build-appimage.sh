#!/usr/bin/env bash
# Builds UinDub-x86_64.AppImage: the latest Qt 6 (aqtinstall), FFmpeg with
# x264, x265 (8, 10 and 12-bit), SVT-AV1, Opus and dav1d, zstd, CPython,
# VapourSynth, the QTGMC plugins and scripts, all from source at their newest
# release. Then UinDub itself, the tests, and the AppImage with the static
# type 2 runtime. Nothing is pinned, so every build picks up upstream fixes.
#
# CI runs this inside ubuntu:22.04. It runs on a desktop too:
#   bash packaging/build-appimage.sh
# Environment:
#   WORK=dir            build folder (default: build-appimage)
#   USE_SYSTEM_QT=1     use the qmake6 on PATH instead of downloading Qt
#   SYSTEM_CODECS=1     use the distro's x264, x265, SVT-AV1, Opus and dav1d instead of building them
#   SKIP_TESTS=1        skip ctest
#   SKIP_DEPS=1         reuse everything already in $WORK/prefix and $WORK/python
#   JOBS=n              parallel jobs (default: all cores)
#   CC=..., CXX=...     compilers for everything (default: cc, c++; must know -march=znver4)
#   UPDATE_INFO=...     zsync update string (default: gh-releases-zsync for NullAngst/UinDub)
set -euo pipefail
export LC_ALL=C

# Every build below takes the compiler from CC and CXX. CI sets gcc-12 and
# g++-12 on Ubuntu 22.04: they know Zen 4, and 22.04's own libstdc++ comes
# from GCC 12, so nothing they build needs a newer one than the host has.
export CC=${CC:-cc}
export CXX=${CXX:-c++}

missing=()
for c in git "$CC" "$CXX" make ninja pkg-config python3 curl nasm patchelf file autoreconf ldd find sort getconf; do
    command -v "$c" >/dev/null 2>&1 || missing+=("$c")
done
if [ "${#missing[@]}" -gt 0 ]; then
    printf 'Missing commands: %s\n' "${missing[*]}" >&2
    printf 'On Ubuntu: apt install build-essential ninja-build pkg-config python3-venv curl nasm patchelf file autoconf automake libtool\n' >&2
    exit 1
fi

# VapourSynth builds one set of kernels with -march=znver4. GCC learned Zen 4
# in 12.3 (and 11.5), Clang in 16; Ubuntu 22.04's default GCC 11.4 did not.
# Checked now so an old compiler fails here, not 40 minutes in.
ZTEST=$(mktemp "${TMPDIR:-/tmp}/uindub-znver4.XXXXXX")
trap 'rm -f -- "$ZTEST"' EXIT
if ! printf 'int main(void) { return 0; }\n' | "$CXX" -march=znver4 -x c++ - -o "$ZTEST" >/dev/null 2>&1; then
    printf '%s does not know -march=znver4, which VapourSynth needs.\n' "$CXX" >&2
    printf 'Use GCC 12.3 or newer: on Ubuntu 22.04, apt install gcc-12 g++-12 and run with CC=gcc-12 CXX=g++-12.\n' >&2
    exit 1
fi
printf 'Compilers: %s, %s\n' "$("$CC" --version | head -n 1)" "$("$CXX" --version | head -n 1)"

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
WORK=${WORK:-$ROOT/build-appimage}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd -P)
PREFIX=$WORK/prefix
PY=$WORK/python
SRC=$WORK/src
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
UPDATE_INFO=${UPDATE_INFO:-"gh-releases-zsync|NullAngst|UinDub|latest|UinDub-*x86_64.AppImage.zsync"}
export ARCH=x86_64
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:$PREFIX/lib/x86_64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}
export LD_LIBRARY_PATH=$PREFIX/lib:$PREFIX/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}
export PATH=$PREFIX/bin:$PATH
mkdir -p "$PREFIX" "$SRC"

step() { printf '\n=== %s ===\n' "$*"; }

# Newest release tag of a git repo whose name matches an extended regex.
# sort -V is a GNU (and BusyBox) extension; there is no POSIX version sort.
latest_tag() {
    git ls-remote --tags --refs "$1" | sed 's|.*refs/tags/||' | grep -E "$2" | sort -V | tail -n 1
}

fetch_tag() {
    local url=$1 tag=$2 dir=$3
    rm -rf -- "$dir"
    git -c advice.detachedHead=false clone -q --depth 1 --recurse-submodules --shallow-submodules --branch "$tag" "$url" "$dir"
}

# --- Build tools -----------------------------------------------------------------
# Current Qt, VapourSynth and the plugins want newer CMake and Meson than
# Ubuntu 22.04 ships, so the latest of each goes into a venv here.
step "Build tools"
TOOLS=$WORK/tools
[ -x "$TOOLS/bin/python3" ] || python3 -m venv "$TOOLS"
"$TOOLS/bin/python3" -m pip install --quiet --upgrade pip
"$TOOLS/bin/python3" -m pip install --quiet --upgrade cmake meson aqtinstall
export PATH=$TOOLS/bin:$PATH
hash -r
cmake --version | head -n 1
printf 'meson %s\n' "$(meson --version)"

# --- Qt ----------------------------------------------------------------------------
step "Qt 6"
if [ "${USE_SYSTEM_QT:-0}" = "1" ]; then
    QMAKE=$(command -v qmake6 || command -v qmake)
    QT_DIR=$("$QMAKE" -query QT_INSTALL_PREFIX)
else
    QT_VER=$(python3 -m aqt list-qt linux desktop --spec 6 --latest-version)
    # Qt 6.7 renamed the desktop arch from gcc_64 to linux_gcc_64.
    QT_ARCH=$(python3 -m aqt list-qt linux desktop --arch "$QT_VER" | tr ' ' '\n' | grep -E '^(linux_)?gcc_64$' | head -n 1)
    printf 'Qt %s (%s)\n' "$QT_VER" "$QT_ARCH"
    python3 -m aqt install-qt linux desktop "$QT_VER" "$QT_ARCH" --outputdir "$WORK/qt"
    QT_DIR=$(find "$WORK/qt/$QT_VER" -mindepth 1 -maxdepth 1 -type d | head -n 1)
    QMAKE=$QT_DIR/bin/qmake
fi
printf 'Qt prefix: %s\n' "$QT_DIR"
QT_PLUGINS=$("$QMAKE" -query QT_INSTALL_PLUGINS)
ls "$QT_PLUGINS"/platforms/libqwayland*.so >/dev/null 2>&1 || { printf 'Qt has no Wayland platform plugin\n' >&2; exit 1; }
ls "$QT_PLUGINS"/platforms/libqxcb.so >/dev/null
export QMAKE
# linuxdeploy resolves libraries through the loader path, and a downloaded Qt
# sits outside it. Appended so $PREFIX still wins.
QT_LIBS=$("$QMAKE" -query QT_INSTALL_LIBS)
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$QT_LIBS

if [ "${SKIP_DEPS:-0}" != "1" ]; then
# --- zstd --------------------------------------------------------------------------
step "zstd"
TAG=$(latest_tag https://github.com/facebook/zstd.git '^v[0-9]+(\.[0-9]+)+$')
printf 'zstd %s\n' "$TAG"
fetch_tag https://github.com/facebook/zstd.git "$TAG" "$SRC/zstd"
cmake -S "$SRC/zstd/build/cmake" -B "$SRC/zstd/out" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_STATIC=OFF -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_TESTS=OFF
cmake --build "$SRC/zstd/out"
cmake --install "$SRC/zstd/out"

if [ "${SYSTEM_CODECS:-0}" != "1" ]; then
# --- x264 --------------------------------------------------------------------------
# x264 tags no releases; its stable branch is the release line.
step "x264"
fetch_tag https://code.videolan.org/videolan/x264.git stable "$SRC/x264"
(
    cd "$SRC/x264" || exit 1
    ./configure --prefix="$PREFIX" --enable-shared --enable-pic --disable-cli --disable-lavf --disable-swscale \
        --disable-ffms --disable-gpac --disable-lsmash
    make -j"$JOBS"
    make install
)

# --- x265, multilib ----------------------------------------------------------------
# One libx265 with the 8, 10 and 12-bit encoders inside, the usual recipe:
# 12 and 10-bit as static libraries, linked into the 8-bit shared one.
step "x265"
TAG=$(latest_tag https://bitbucket.org/multicoreware/x265_git.git '^[0-9]+(\.[0-9]+)+$')
printf 'x265 %s\n' "$TAG"
fetch_tag https://bitbucket.org/multicoreware/x265_git.git "$TAG" "$SRC/x265"
X265_COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DENABLE_CLI=OFF
             -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
cmake -S "$SRC/x265/source" -B "$SRC/x265/b12" "${X265_COMMON[@]}" -DHIGH_BIT_DEPTH=ON -DMAIN12=ON -DEXPORT_C_API=OFF -DENABLE_SHARED=OFF
cmake --build "$SRC/x265/b12"
cmake -S "$SRC/x265/source" -B "$SRC/x265/b10" "${X265_COMMON[@]}" -DHIGH_BIT_DEPTH=ON -DEXPORT_C_API=OFF -DENABLE_SHARED=OFF
cmake --build "$SRC/x265/b10"
mkdir -p "$SRC/x265/b8"
cp -- "$SRC/x265/b12/libx265.a" "$SRC/x265/b8/libx265_main12.a"
cp -- "$SRC/x265/b10/libx265.a" "$SRC/x265/b8/libx265_main10.a"
cmake -S "$SRC/x265/source" -B "$SRC/x265/b8" "${X265_COMMON[@]}" -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    -DEXTRA_LIB="x265_main10.a;x265_main12.a" -DEXTRA_LINK_FLAGS=-L. -DLINKED_10BIT=ON -DLINKED_12BIT=ON \
    -DENABLE_SHARED=ON -DENABLE_HDR10_PLUS=OFF
cmake --build "$SRC/x265/b8"
cmake --install "$SRC/x265/b8"
rm -f -- "$PREFIX/lib/libx265.a"

# --- SVT-AV1 -----------------------------------------------------------------------
step "SVT-AV1"
TAG=$(latest_tag https://gitlab.com/AOMediaCodec/SVT-AV1.git '^v[0-9]+(\.[0-9]+)+$')
printf 'SVT-AV1 %s\n' "$TAG"
fetch_tag https://gitlab.com/AOMediaCodec/SVT-AV1.git "$TAG" "$SRC/svtav1"
cmake -S "$SRC/svtav1" -B "$SRC/svtav1/out" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=ON -DBUILD_APPS=OFF -DBUILD_TESTING=OFF -DBUILD_DEC=OFF
cmake --build "$SRC/svtav1/out"
cmake --install "$SRC/svtav1/out"

# --- Opus --------------------------------------------------------------------------
step "Opus"
TAG=$(latest_tag https://github.com/xiph/opus.git '^v[0-9]+(\.[0-9]+)+$')
printf 'Opus %s\n' "$TAG"
fetch_tag https://github.com/xiph/opus.git "$TAG" "$SRC/opus"
cmake -S "$SRC/opus" -B "$SRC/opus/out" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=ON -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF
cmake --build "$SRC/opus/out"
cmake --install "$SRC/opus/out"

# --- dav1d -------------------------------------------------------------------------
# AV1 decoding, so an AV1 file opens as a source like anything else.
step "dav1d"
TAG=$(latest_tag https://code.videolan.org/videolan/dav1d.git '^[0-9]+(\.[0-9]+)+$')
printf 'dav1d %s\n' "$TAG"
fetch_tag https://code.videolan.org/videolan/dav1d.git "$TAG" "$SRC/dav1d"
meson setup "$SRC/dav1d/out" "$SRC/dav1d" --buildtype=release --prefix="$PREFIX" --libdir=lib \
    -Denable_tools=false -Denable_tests=false
ninja -C "$SRC/dav1d/out"
ninja -C "$SRC/dav1d/out" install
fi

# --- FFmpeg ------------------------------------------------------------------------
# GPL build: x264, x265 and the fieldmatch/decimate IVTC filters are GPL.
# Autodetect is off so no host library (VAAPI, X11, SDL) sneaks in.
step "FFmpeg"
TAG=$(latest_tag https://github.com/FFmpeg/FFmpeg.git '^n[0-9]+(\.[0-9]+)+$')
printf 'FFmpeg %s\n' "$TAG"
fetch_tag https://github.com/FFmpeg/FFmpeg.git "$TAG" "$SRC/ffmpeg"
(
    cd "$SRC/ffmpeg" || exit 1
    ./configure --prefix="$PREFIX" --cc="$CC" --cxx="$CXX" --enable-gpl --enable-version3 --enable-shared --disable-static --disable-doc \
        --disable-debug --disable-autodetect --disable-network --disable-ffplay --enable-zlib \
        --enable-libx264 --enable-libx265 --enable-libsvtav1 --enable-libopus --enable-libdav1d \
        --extra-cflags="-I$PREFIX/include" --extra-ldflags="-L$PREFIX/lib"
    make -j"$JOBS"
    make install
)

# --- CPython -----------------------------------------------------------------------
# VapourSynth runs inside UinDub through VSScript, which embeds Python. The
# AppImage carries its own, so the host's Python version never matters.
step "Python"
TAG=$(latest_tag https://github.com/python/cpython.git '^v3\.[0-9]+\.[0-9]+$')
printf 'Python %s\n' "$TAG"
fetch_tag https://github.com/python/cpython.git "$TAG" "$SRC/cpython"
(
    cd "$SRC/cpython" || exit 1
    ./configure --prefix="$PY" --enable-shared --with-ensurepip=install --without-static-libpython >/dev/null
    make -j"$JOBS"
    make install >/dev/null
)
# $ORIGIN keeps bin/python3 finding libpython wherever the tree goes, here and
# in the AppDir. Set after the build, since Make would eat the $ in LDFLAGS.
for f in "$PY"/bin/python3.*; do
    case "$f" in *-config) continue ;; esac
    [ -L "$f" ] || patchelf --set-rpath "\$ORIGIN/../lib" "$f"
done
PYBIN=$PY/bin/python3
"$PYBIN" -m pip install --quiet --upgrade pip

# --- VapourSynth -------------------------------------------------------------------
# Built with meson-python into the bundled Python; zimg comes in as a Meson
# subproject at its newest commit. VapourSynth needs Vulkan headers newer than
# most distros ship, and takes the system's over its own copy when it finds
# any, so the newest Khronos headers go first on the include path.
# Declarations only: nothing links against Vulkan.
step "Vulkan headers"
TAG=$(latest_tag https://github.com/KhronosGroup/Vulkan-Headers.git '^v1\.[0-9]+\.[0-9]+$')
printf 'Vulkan-Headers %s\n' "$TAG"
fetch_tag https://github.com/KhronosGroup/Vulkan-Headers.git "$TAG" "$SRC/vulkan-headers"
cmake -S "$SRC/vulkan-headers" -B "$SRC/vulkan-headers/out" -G Ninja -DCMAKE_INSTALL_PREFIX="$WORK/vulkan" \
    -DVULKAN_HEADERS_ENABLE_MODULE=OFF -DVULKAN_HEADERS_ENABLE_TESTS=OFF
cmake --install "$SRC/vulkan-headers/out"

step "VapourSynth"
TAG=$(latest_tag https://github.com/vapoursynth/vapoursynth.git '^R[0-9]+$')
printf 'VapourSynth %s\n' "$TAG"
fetch_tag https://github.com/vapoursynth/vapoursynth.git "$TAG" "$SRC/vapoursynth"
CPLUS_INCLUDE_PATH="$WORK/vulkan/include" C_INCLUDE_PATH="$WORK/vulkan/include" \
    "$PYBIN" -m pip install --quiet --no-cache-dir "$SRC/vapoursynth"
fi

PYBIN=$PY/bin/python3
VSPKG=$("$PYBIN" -c 'import os, vapoursynth; print(os.path.dirname(vapoursynth.__file__))')
SITE=$(dirname "$VSPKG")
VSINC=$VSPKG/include
PLUGDIR=$VSPKG/plugins
printf 'VapourSynth package: %s\n' "$VSPKG"
# Plugin builds ask Python where VapourSynth's headers are. Meson asks the
# Python it runs under unless a native file names another, so it gets one.
export PATH=$PY/bin:$PATH
hash -r
NATIVE=$WORK/meson-native.ini
printf "[binaries]\npython = '%s'\npython3 = '%s'\n" "$PYBIN" "$PYBIN" >"$NATIVE"

if [ "${SKIP_DEPS:-0}" != "1" ]; then
# --- QTGMC plugins -----------------------------------------------------------------
# What havsfunc's QTGMC calls: mvtools, znedi3 (with the nnedi3 weights),
# removegrain (rgvs), fmtconv, eedi3m, and the misc filters VapourSynth moved
# out of its core. Plugins in vapoursynth/plugins load automatically.
step "QTGMC plugins"
mkdir -p "$PLUGDIR"
export PKG_CONFIG_PATH=$VSPKG/pkgconfig:$PKG_CONFIG_PATH
PLUG_SRC=$SRC/plugins
mkdir -p "$PLUG_SRC"
# Newer GCC no longer pulls <cmath> and <cfloat> in through other headers,
# which some of these sources rely on.
PLUG_CXXFLAGS="-O2 -include math.h -include cfloat"

TAG=$(latest_tag https://github.com/dubhater/vapoursynth-mvtools.git '^v[0-9]+(_[0-9]+)?$')
printf 'mvtools %s\n' "$TAG"
fetch_tag https://github.com/dubhater/vapoursynth-mvtools.git "$TAG" "$PLUG_SRC/mvtools"
CXXFLAGS=$PLUG_CXXFLAGS meson setup "$PLUG_SRC/mvtools/build" "$PLUG_SRC/mvtools" --buildtype=release --native-file "$NATIVE"
ninja -C "$PLUG_SRC/mvtools/build"
find "$PLUG_SRC/mvtools/build" -maxdepth 1 -name '*.so' -exec cp -- {} "$PLUGDIR/" \;

TAG=$(latest_tag https://github.com/sekrit-twc/znedi3.git '^r[0-9]+(\.[0-9]+)*$')
printf 'znedi3 %s\n' "$TAG"
fetch_tag https://github.com/sekrit-twc/znedi3.git "$TAG" "$PLUG_SRC/znedi3"
make -C "$PLUG_SRC/znedi3" -j"$JOBS" X86=1 CC="$CC" CXX="$CXX"
cp -- "$PLUG_SRC/znedi3/vsznedi3.so" "$PLUG_SRC/znedi3/nnedi3_weights.bin" "$PLUGDIR/"

TAG=$(latest_tag https://github.com/vapoursynth/vs-removegrain.git '^R[0-9]+$')
printf 'removegrain %s\n' "$TAG"
fetch_tag https://github.com/vapoursynth/vs-removegrain.git "$TAG" "$PLUG_SRC/removegrain"
# shellcheck disable=SC2086  # PLUG_CXXFLAGS is a list of flags
"$CXX" -std=c++17 $PLUG_CXXFLAGS -shared -fPIC -I"$VSINC" "$PLUG_SRC"/removegrain/src/*.cpp -o "$PLUGDIR/libremovegrain.so"

TAG=$(latest_tag https://github.com/vapoursynth/vs-miscfilters-obsolete.git '^R[0-9]+$')
printf 'miscfilters %s\n' "$TAG"
fetch_tag https://github.com/vapoursynth/vs-miscfilters-obsolete.git "$TAG" "$PLUG_SRC/misc"
# It includes VapourSynth's own internal helpers, so it builds against the source tree.
[ -d "$SRC/vapoursynth" ] || fetch_tag https://github.com/vapoursynth/vapoursynth.git \
    "$(latest_tag https://github.com/vapoursynth/vapoursynth.git '^R[0-9]+$')" "$SRC/vapoursynth"
# shellcheck disable=SC2086
"$CXX" -std=c++17 $PLUG_CXXFLAGS -shared -fPIC -DVS_USE_LATEST_API -DVS_GRAPH_API -I"$VSINC" -I"$SRC/vapoursynth/src/core" \
    "$PLUG_SRC"/misc/src/*.cpp -o "$PLUGDIR/libmiscfilters.so"

TAG=$(latest_tag https://github.com/EleonoreMizo/fmtconv.git '^r[0-9]+$')
printf 'fmtconv %s\n' "$TAG"
fetch_tag https://github.com/EleonoreMizo/fmtconv.git "$TAG" "$PLUG_SRC/fmtconv"
(
    cd "$PLUG_SRC/fmtconv/build/unix" || exit 1
    ./autogen.sh >/dev/null 2>&1
    CPPFLAGS="-I$VSINC" ./configure --quiet
    make -j"$JOBS"
)
cp -L -- "$PLUG_SRC/fmtconv/build/unix/.libs/libfmtconv.so" "$PLUGDIR/"

TAG=$(latest_tag https://github.com/HomeOfVapourSynthEvolution/VapourSynth-EEDI3.git '^r[0-9]+(\.[0-9]+)*$')
printf 'eedi3m %s\n' "$TAG"
fetch_tag https://github.com/HomeOfVapourSynthEvolution/VapourSynth-EEDI3.git "$TAG" "$PLUG_SRC/eedi3"
CXXFLAGS=$PLUG_CXXFLAGS meson setup "$PLUG_SRC/eedi3/build" "$PLUG_SRC/eedi3" --buildtype=release --native-file "$NATIVE"
ninja -C "$PLUG_SRC/eedi3/build"
find "$PLUG_SRC/eedi3/build" -maxdepth 1 -name '*.so' -exec cp -- {} "$PLUGDIR/" \;

# --- QTGMC scripts -----------------------------------------------------------------
step "QTGMC scripts"
TAG=$(latest_tag https://github.com/HomeOfVapourSynthEvolution/mvsfunc.git '^r[0-9]+$')
printf 'mvsfunc %s\n' "$TAG"
fetch_tag https://github.com/HomeOfVapourSynthEvolution/mvsfunc.git "$TAG" "$PLUG_SRC/mvsfunc"
cp -- "$(find "$PLUG_SRC/mvsfunc" -name mvsfunc.py | head -n 1)" "$SITE/"

TAG=$(latest_tag https://github.com/dubhater/vapoursynth-adjust.git '^v[0-9]+$')
printf 'adjust %s\n' "$TAG"
fetch_tag https://github.com/dubhater/vapoursynth-adjust.git "$TAG" "$PLUG_SRC/adjust"
cp -- "$PLUG_SRC/adjust/adjust.py" "$SITE/"

# havsfunc dropped QTGMC after r33 (it moved to other packages). Take the
# newest release that still has it, so a future release that brings it back
# is picked up on its own.
HAVS_URL=https://github.com/HomeOfVapourSynthEvolution/havsfunc.git
HAVS=
# sort -V is GNU; see latest_tag.
while IFS= read -r t; do
    fetch_tag "$HAVS_URL" "$t" "$PLUG_SRC/havsfunc"
    f=$(find "$PLUG_SRC/havsfunc" -name havsfunc.py | head -n 1)
    if [ -n "$f" ] && grep -q '^def QTGMC' "$f"; then
        HAVS=$f
        printf 'havsfunc %s (newest with QTGMC)\n' "$t"
        break
    fi
done < <(git ls-remote --tags --refs "$HAVS_URL" | sed 's|.*refs/tags/||' | grep -E '^r[0-9]+$' | sort -V -r)
[ -n "$HAVS" ] || { printf 'No havsfunc release has QTGMC\n' >&2; exit 1; }
# Current VapourSynth rejects the old leading-underscore keywords (_lambda,
# _global) havsfunc passes to mvtools; the trailing-underscore form is the
# same argument.
"$PYBIN" - "$HAVS" "$SITE/havsfunc.py" <<'PYEOF'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
src = re.sub(r"\b_lambda=", "lambda_=", src)
src = re.sub(r"\b_global=", "global_=", src)
open(sys.argv[2], "w", encoding="utf-8").write(src)
PYEOF
fi

# Fail now, not at the user's desk, if QTGMC does not run.
step "QTGMC check"
"$PYBIN" - <<'PYEOF'
import vapoursynth as vs
import havsfunc as haf
core = vs.core
print("plugins:", " ".join(sorted(p.namespace for p in core.plugins())))
clip = core.std.BlankClip(width=720, height=486, format=vs.YUV444P16, length=12, fpsnum=30000, fpsden=1001)
clip = core.std.SetFieldBased(clip, 2)
for preset in ("Draft", "Slower"):
    out = haf.QTGMC(clip, Preset=preset, TFF=True, NoiseProcess=0)
    for i in range(out.num_frames):
        out.get_frame(i)
    print(preset, "ok,", out.num_frames, "frames")
PYEOF

# --- UinDub --------------------------------------------------------------------------
step "UinDub"
BUILD=$WORK/build
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_DIR;$PREFIX" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
    -DUINDUB_LTO=ON -DCMAKE_INSTALL_PREFIX=/usr -DVAPOURSYNTH_INCLUDE="$VSINC"
cmake --build "$BUILD" -j"$JOBS"

# --- Tests ---------------------------------------------------------------------------
if [ "${SKIP_TESTS:-0}" != "1" ]; then
    step "Tests"
    (cd "$BUILD" && UINDUB_PYTHON="$PYBIN" UD_FFPROBE="$PREFIX/bin/ffprobe" ctest --output-on-failure -j"$JOBS")
fi

# --- AppDir with linuxdeploy -----------------------------------------------------------
step "AppDir"
for t in linuxdeploy/linuxdeploy/linuxdeploy-x86_64.AppImage \
         linuxdeploy/linuxdeploy-plugin-qt/linuxdeploy-plugin-qt-x86_64.AppImage \
         AppImage/appimagetool/appimagetool-x86_64.AppImage; do
    f=$TOOLS/${t##*/}
    [ -s "$f" ] || curl -fsSL -o "$f" "https://github.com/${t%/*}/releases/download/continuous/${t##*/}"
    chmod +x "$f"
done
# Containers have no FUSE: run the tool AppImages extracted.
export APPIMAGE_EXTRACT_AND_RUN=1
APPDIR=$WORK/AppDir
rm -rf -- "$APPDIR"
DESTDIR=$APPDIR cmake --install "$BUILD"
PLATFORMS=$(cd "$QT_PLUGINS/platforms" && printf '%s\n' libqwayland*.so libqxcb.so | paste -sd ';' -)
printf 'Platform plugins: %s\n' "$PLATFORMS"
export EXTRA_PLATFORM_PLUGINS=$PLATFORMS
export EXTRA_QT_MODULES="svg"
# Libraries that must come from the host: they are tied to its drivers,
# sound server, D-Bus and udev.
export LINUXDEPLOY_EXCLUDED_LIBRARIES="libasound.so*;libudev.so*;libGL.so*;libEGL.so*;libdbus-1.so*;libGLX*;libOpenGL.so*;libGLdispatch.so*"
EXCLUDES=(--exclude-library 'libasound.so*' --exclude-library 'libudev.so*' --exclude-library 'libGL.so*'
          --exclude-library 'libEGL.so*' --exclude-library 'libdbus-1.so*' --exclude-library 'libGLX*'
          --exclude-library 'libOpenGL.so*' --exclude-library 'libGLdispatch.so*')
"$TOOLS/linuxdeploy-x86_64.AppImage" --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/uindub" \
    --desktop-file "$APPDIR/usr/share/applications/io.github.nullangst.UinDub.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/io.github.nullangst.UinDub.svg" \
    "${EXCLUDES[@]}" \
    --plugin qt

for d in wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client; do
    if [ -d "$QT_PLUGINS/$d" ] && [ ! -d "$APPDIR/usr/plugins/$d" ]; then
        printf 'Adding missing Qt plugin folder %s\n' "$d"
        mkdir -p "$APPDIR/usr/plugins"
        cp -R -P -p -- "$QT_PLUGINS/$d" "$APPDIR/usr/plugins/"
    fi
done
ls "$APPDIR"/usr/plugins/platforms/

# --- The QTGMC stack -------------------------------------------------------------------
# Copied in after linuxdeploy, which would otherwise rewrite the RUNPATH of
# VapourSynth's Python module ($ORIGIN, where libvapoursynth sits). UinDub
# finds it at usr/lib/uindub/python next to its own usr/bin.
step "Python and VapourSynth into the AppDir"
APY=$APPDIR/usr/lib/uindub/python
mkdir -p "$APY"
cp -R -P -p -- "$PY/bin" "$PY/lib" "$APY/"
PYVER=$("$PYBIN" -c 'import sys; print("%d.%d" % sys.version_info[:2])')
# What a frameserver never needs: tests, IDLE, Tk, headers, bytecode caches.
rm -rf -- "$APY/lib/python$PYVER/test" "$APY/lib/python$PYVER/idlelib" "$APY/lib/python$PYVER/tkinter" \
    "$APY/lib/python$PYVER/turtledemo" "$APY/lib/python$PYVER/ensurepip" "$APY/lib/python$PYVER/config-$PYVER"* \
    "$APY/lib/pkgconfig" "$APY/lib/python$PYVER/site-packages/pip"
find "$APY" -name '__pycache__' -type d -prune -exec rm -rf -- {} +
find "$APY" -name '*.a' -type f -exec rm -f -- {} +

# Bundle what the stack links against, beside Qt's libraries in usr/lib, and
# point each file's RUNPATH there too (keeping its own $ORIGIN entries). The
# C and C++ runtimes are left to the host like linuxdeploy does, since UinDub
# already loaded them by the time Python starts.
is_elf() { [ "$(od -An -c -N 4 -- "$1" | tr -d ' ')" = '177ELF' ]; }
host_lib() {
    case "${1##*/}" in
        libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libutil.so*|libresolv.so*|ld-linux*|libgcc_s.so*|libstdc++.so*) return 0 ;;
        libasound.so*|libudev.so*|libGL.so*|libEGL.so*|libdbus-1.so*|libGLX*|libOpenGL.so*|libGLdispatch.so*) return 0 ;;
    esac
    return 1
}
relpath() { "$PYBIN" -c 'import os, sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' "$1" "$2"; }
fix_elf() {
    local f=$1 dep path old rel
    while IFS= read -r line; do
        case "$line" in *' => /'*) ;; *) continue ;; esac
        dep=${line%% =>*}
        dep=${dep#"${dep%%[![:space:]]*}"}
        path=${line#*=> }
        path=${path%% (*}
        case "$path" in "$APPDIR"/*) continue ;; esac
        host_lib "$dep" && continue
        if [ ! -e "$APPDIR/usr/lib/$dep" ]; then
            printf '  bundling %s (for %s)\n' "$dep" "${f#"$APPDIR"/}"
            cp -L -- "$path" "$APPDIR/usr/lib/$dep"
            chmod u+w -- "$APPDIR/usr/lib/$dep"
            fix_elf "$APPDIR/usr/lib/$dep"
        fi
    done < <(ldd "$f" 2>/dev/null || true)
    rel=$(relpath "$APPDIR/usr/lib" "$(dirname "$f")")
    old=$(patchelf --print-rpath "$f" 2>/dev/null || true)
    case ":$old:" in *":\$ORIGIN/$rel:"*) return 0 ;; esac
    patchelf --set-rpath "${old:+$old:}\$ORIGIN/$rel" "$f"
}
while IFS= read -r -d '' f; do
    is_elf "$f" || continue
    chmod u+w -- "$f"
    fix_elf "$f"
done < <(find "$APY" -type f \( -name '*.so' -o -name '*.so.*' -o -path '*/bin/python3*' \) -print0)

# The bundled stack must load with nothing from the build machine.
step "Bundled QTGMC check"
env -i HOME="$WORK" PATH=/usr/bin:/bin PYTHONHOME="$APY" PYTHONNOUSERSITE=1 LD_LIBRARY_PATH="$APPDIR/usr/lib" \
    "$APY/bin/python3" -c 'import vapoursynth as vs, havsfunc; print("bundled VapourSynth", vs.core.core_version)'

# Fail if a host-only library slipped in.
BAD=$(find "$APPDIR" \( -name 'libasound.so*' -o -name 'libudev.so*' -o -name 'libGL.so*' -o -name 'libEGL.so*' \
    -o -name 'libdbus-1.so*' -o -name 'libc.so*' -o -name 'libc-*.so' -o -name 'ld-linux*' \) -print)
if [ -n "$BAD" ]; then
    printf 'Host-only libraries found in the AppDir:\n%s\n' "$BAD" >&2
    exit 1
fi
printf 'AppDir check passed: no libasound, libudev, libGL, libEGL, libdbus-1 or libc inside.\n'

# --- AppImage with the static runtime and update info -----------------------------
step "AppImage"
VERSION=$(git -C "$ROOT" describe --tags --always 2>/dev/null | sed 's/^v//' || printf 'dev')
export VERSION
OUT=$ROOT/UinDub-x86_64.AppImage
rm -f -- "$OUT" "$OUT.zsync"
# appimagetool from AppImage/appimagetool embeds the static type 2 runtime,
# so the host needs no libfuse2.
(cd "$ROOT" && "$TOOLS/appimagetool-x86_64.AppImage" -u "$UPDATE_INFO" "$APPDIR" "$OUT")
if [ ! -f "$OUT.zsync" ] && command -v zsyncmake >/dev/null 2>&1; then
    (cd "$ROOT" && zsyncmake -u UinDub-x86_64.AppImage UinDub-x86_64.AppImage)
fi
ls -la "$OUT"*
printf '\nDone: %s\n' "$OUT"
