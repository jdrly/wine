#!/bin/zsh
# gw2 (jdrly/gw2-on-mac#40): build dlls/ntdll/ntdll.so the way Mach9 0.3.1's CI built the engine's
# (athei/mach9 at v0.3.1: bin/build-wine.sh, bin/lib/profile.sh, .github/workflows/release.yml).
#
#   ntdll.zsh prepare <commit>    check $SRC out at <commit> and give it the wine-11.21 tag
#   ntdll.zsh checkout <commit>   move $SRC to another commit
#   ntdll.zsh configure           configure a tree in $BUILD for $SRC
#   ntdll.zsh build               build dlls/ntdll/ntdll.so in $BUILD and check its version string
#
# $SRC is a clone of this repository with its whole history, as the workflow checks it out. $SRC
# and $BUILD are the engine's own paths (its debug map names them), since the paths reach the
# file. The configure arguments are Mach9's, except that GStreamer, OpenGL and Vulkan are off: Mach9
# builds those itself, and nothing under dlls/ntdll/unix reads what they set. The baseline compare
# in .github/workflows/gw2.yml decides whether that holds.
emulate -L zsh
setopt err_exit pipe_fail no_unset extended_glob

: ${SRC:=/Users/runner/work/mach9/mach9/src} ${BUILD:=/Users/runner/work/mach9/mach9/build/wine}
brew=/opt/homebrew

prepare() {
  local commit=$1 version
  git -C "$SRC" checkout --quiet --detach "$commit"
  # The fork has no Wine release tags, so git describe would find none: Mach9's CI fetches the tag
  # of the release the source is based on from Wine's mirror the same way.
  version=$(sed -n 's/^Wine version //p' "$SRC/VERSION")
  git -C "$SRC" fetch --quiet --no-tags https://github.com/wine-mirror/wine \
    "refs/tags/wine-$version:refs/tags/wine-$version"
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
    --without-opengl --with-sdl --without-vulkan
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
  )
  mkdir -p "$BUILD"
  cd "$BUILD"
  "$SRC/configure" "${args[@]}"
}

build() {
  local file=$BUILD/dlls/ntdll/ntdll.so commit=$(git -C "$SRC" rev-parse HEAD) version found hash
  make -C "$BUILD" -j"$(sysctl -n hw.ncpu)" dlls/ntdll/ntdll.so
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
  *) print -u2 "usage: ntdll.zsh prepare <commit> | checkout <commit> | configure | build"; exit 2 ;;
esac
