#!/bin/zsh
# gw2 (jdrly/gw2-on-mac#40, #57, #61): build dlls/ntdll/ntdll.so, dlls/winemac.drv/winemac.so and
# dlls/win32u/win32u.so the way Mach9 0.3.1's CI built the engine's (athei/mach9 at v0.3.1: bin/build-wine.sh,
# bin/lib/profile.sh, .github/workflows/release.yml).
#
#   wine.zsh prepare <commit>    check $SRC out at <commit> and give it the wine-11.21 tag
#   wine.zsh checkout <commit>   move $SRC to another commit
#   wine.zsh configure           configure a tree in $BUILD for $SRC
#   wine.zsh build               build ntdll.so, winemac.so and win32u.so in $BUILD and check
#                                ntdll.so's version string
#
# $MINGW_DIR is llvm-mingw. $SRC is a clone of this repository with its whole history, as the workflow checks it out. $SRC
# and $BUILD are the engine's own paths (its debug map names them), since the paths reach the
# file. The configure arguments are Mach9's, except that GStreamer is off: Mach9 builds it itself,
# and nothing under dlls/ntdll/unix, dlls/winemac.drv or dlls/win32u reads what it sets. OpenGL and
# Vulkan are on without their libraries: of what they set, win32u.so needs only the names it loads
# EGL and Vulkan by (SONAME_LIBEGL, SONAME_LIBVULKAN), and FreeType's (SONAME_LIBFREETYPE). Mach9
# found the three in its Mesa build and Homebrew and then rewrote them in config.h to the bundle's
# paths (athei/mach9 at v0.3.1, bin/build-wine.sh: patch_sonames). Here configure's cache variables
# give it those paths, so it never links against the libraries, and configure below checks
# config.h for them. FreeType's headers are Homebrew's, as Mach9's were (2.15.0 in its release
# run). The baseline compare in .github/workflows/gw2.yml decides whether that holds.
emulate -L zsh
setopt err_exit pipe_fail no_unset extended_glob

: ${SRC:=/Users/runner/work/mach9/mach9/src} ${BUILD:=/Users/runner/work/mach9/mach9/build/wine}
: ${MINGW_DIR:?the llvm-mingw directory}
brew=/opt/homebrew
# where the engine's unix modules find the libraries Mach9 bundles
bundled=@loader_path/../../external
# appended, as Mach9 does: llvm-mingw's unprefixed clang mustn't replace Apple's
path+=("$MINGW_DIR/bin")

prepare() {
  local commit=$1 version
  git -C "$SRC" checkout --quiet --detach "$commit"
  # The fork has no Wine release tags, so git describe would find none: Mach9's CI fetches the tag
  # of the release the source is based on from Wine's mirror the same way.
  version=$(sed -n 's/^Wine version //p' "$SRC/VERSION")
  git -C "$SRC" fetch --quiet --no-tags https://github.com/wine-mirror/wine \
    "refs/tags/wine-${version}:refs/tags/wine-${version}"
  # The engine's string has 10 digits of the commit (wine-11.21-37-g53def89293). git picks the
  # length from the repository's object count, which isn't the same in my fork as in athei's.
  git -C "$SRC" config core.abbrev 10
  git -C "$SRC" describe HEAD
}

checkout() {
  git -C "$SRC" checkout --quiet --detach "$1"
  git -C "$SRC" describe HEAD
}

configure() {
  local args=(
    --enable-archs=arm64ec,aarch64,i386
    --with-coreaudio --with-freetype --with-gnutls --without-gstreamer --with-mingw --with-opencl
    --with-opengl --with-sdl --with-vulkan
    --without-alsa --without-capi --without-cups --without-dbus --without-ffmpeg --without-fontconfig
    --without-gettext --without-gphoto --without-gssapi --without-hwloc --without-inotify --without-krb5
    --without-netapi --without-oss --without-pcap --without-pcsclite --without-pulse --without-sane
    --without-udev --without-usb --without-v4l2 --without-va --without-wayland --without-x
    --without-xcomposite --without-xcursor --without-xfixes --without-xinerama --without-xinput
    --without-xinput2 --without-xrandr --without-xrender --without-xshape --without-xshm
    --without-xxf86vm
    CC="clang -arch arm64" --host=aarch64-apple-darwin --disable-tests
    BISON="$brew/opt/bison/bin/bison" PKG_CONFIG_PATH="$brew/lib/pkgconfig"
    CPPFLAGS="-I$brew/include" LDFLAGS="-L$brew/lib"
    # macOS 26.4 has no pipe2 (Mach9's FLOOR_NEWER_FUNCS)
    ac_cv_func_pipe2=no
    # the names Mach9's config.h ends up with (WINE_CHECK_SONAME takes a cached value as found)
    ac_cv_lib_soname_EGL=$bundled/libEGL.1.dylib
    ac_cv_lib_soname_freetype=$bundled/libfreetype.6.dylib
    ac_cv_lib_soname_vulkan=$bundled/libvulkan.1.dylib
  )
  local def want found
  mkdir -p "$BUILD"
  cd "$BUILD"
  "$SRC/configure" "${args[@]}"
  for def want in SONAME_LIBEGL libEGL.1.dylib SONAME_LIBFREETYPE libfreetype.6.dylib SONAME_LIBVULKAN libvulkan.1.dylib; do
    found=$(sed -n "s/^#define $def //p" include/config.h)
    [[ $found == "\"$bundled/$want\"" ]] || { print -u2 "config.h: $def is ${found:-not defined}, not \"$bundled/$want\""; exit 1 }
    print "config.h: $def $found"
  done
}

build() {
  local file=$BUILD/dlls/ntdll/ntdll.so commit=$(git -C "$SRC" rev-parse HEAD) version found hash
  make -C "$BUILD" -j"$(sysctl -n hw.ncpu)" dlls/ntdll/ntdll.so dlls/winemac.drv/winemac.so dlls/win32u/win32u.so
  # wine_build, from git describe (configure.ac's rule for dlls/ntdll/unix/version.c). It follows
  # a 16-byte table without a NUL, so strings prints the two as one.
  version=$(sed -n 's/^Wine version //p' "$SRC/VERSION")
  found=(${(f)"$(strings -a "$file" | grep -oE 'wine-[0-9]+\.[0-9]+[-0-9a-g]*' | sort -u || true)"})
  hash=${found[1]:-}
  hash=${hash##*-g}
  if (( ${#found} == 1 )) && [[ $found[1] == wine-$version-<->-g[0-9a-f]## && ${#hash} -ge 7 && $commit == $hash* ]]; then
    print "ntdll.so says $found[1], for $commit"
  else
    print -u2 "ntdll.so says ${found:-no version}, not wine-$version-<n>-g<commit> for $commit"
    exit 1
  fi
}

case ${1:-}:$# in
  prepare:2 | checkout:2 | configure:1 | build:1) "$@" ;;
  *) print -u2 "usage: wine.zsh prepare <commit> | checkout <commit> | configure | build"; exit 2 ;;
esac
