#!/bin/zsh
# gw2 (jdrly/gw2-on-mac#387): my mmdevapi.dll, the fork's one PE file, staged as Mach9 stages the
# engine's and checked against it. compare.py reads Mach-O files only.
#
#   pe.zsh stage <built> <out>               strip <built> into <out> as Mach9 strips every PE file
#                                            of the engine (athei/mach9 at v0.4.0,
#                                            bin/bundle-wine.sh, Step 4e: llvm-strip --strip-all),
#                                            and check Wine's builtin marker survived
#   pe.zsh shape <file>                      print what the gate holds a file to: its machine
#                                            (both views of an ARM64X file), its sections by name,
#                                            its exports by ordinal and name, and its imports and
#                                            delay imports by DLL and name. Not the sections'
#                                            sizes: a change may grow one
#   pe.zsh compare <file> <reference>        the gate: <file>'s shape is <reference>'s. Then a
#                                            report, never a gate: how many bytes differ, and the
#                                            first of them. The link stamps the file with the time
#                                            and with a hash of its contents, so no two builds
#                                            match byte for byte.
#   pe.zsh objects <base> <head> <name>...   the gate: <base> and <head> hold the same PE objects
#                                            of one module, a directory per architecture as the
#                                            build leaves them (aarch64-windows/, arm64ec-windows/),
#                                            and every object but the <name>s (file names, such as
#                                            audiosessionmanager.o) is the same, byte for byte
#
# Each prints Markdown for the job summary. llvm-readobj and llvm-strip are $MINGW_DIR's, the
# llvm-mingw Mach9 builds its PE files with.
emulate -L zsh
setopt err_exit pipe_fail no_unset extended_glob

: ${MINGW_DIR:?the llvm-mingw directory}
readobj=$MINGW_DIR/bin/llvm-readobj

# grep without -q, which would end the pipe early and fail it under pipe_fail
builtin_marker() { head -c 128 "$1" | grep -a 'Wine builtin DLL' >/dev/null }

stage() {
  local built=$1 out=$2
  builtin_marker "$built" || { print -u2 "$built has no Wine builtin marker"; exit 1 }
  "$MINGW_DIR/bin/llvm-strip" --strip-all -o "$out" "$built"
  builtin_marker "$out" || { print -u2 "stripping $built lost its Wine builtin marker"; exit 1 }
  if "$MINGW_DIR/bin/llvm-objdump" -h "$out" | grep ' \.debug_' >/dev/null; then
    print -u2 "$out still has .debug_* sections"
    exit 1
  fi
}

shape() {
  local f=$1
  "$readobj" --file-headers "$f" | sed -nE 's/^ *(Machine: .*)$/\1/p'
  "$readobj" --sections "$f" | awk '/^ *Name: / { print "section " $2 }'
  "$readobj" --coff-exports "$f" | awk '/^ *Ordinal: / { o = $2 } /^ *Name: / { print "export " o " " $2 }'
  "$readobj" --coff-imports "$f" | awk '
    /^ *(Import|DelayImport) \{/ { kind = $1 }
    /^ *Name: / { dll = $2 }
    /^ *Symbol: / { $1 = ""; print kind " " dll $0 }'
}

compare() {
  local f=$1 ref=$2 n
  local a=${TMPDIR:-/tmp}/pe-shape-$$-a.txt b=${TMPDIR:-/tmp}/pe-shape-$$-b.txt
  shape "$f" >"$a"
  shape "$ref" >"$b"
  print "### ${f:t} against ${ref:t} (${ref:h:t}/)"
  print '```'
  if diff "$b" "$a"; then
    print "shape: the same, $(grep -c . "$a") lines (machine, sections, exports, imports)"
    rm -f "$a" "$b"
  else
    print "shape: differs (< the reference, > mine)"
    print '```'
    rm -f "$a" "$b"
    return 1
  fi
  n=$(cmp -l "$f" "$ref" | wc -l | tr -d ' ' || true)
  print "bytes: $(wc -c <"$f" | tr -d ' ') against $(wc -c <"$ref" | tr -d ' '), $n differ (a report: the link's time and content stamps differ in every build)"
  (( n == 0 )) || { print "first differing offsets (decimal, mine, the reference's, octal):"; cmp -l "$f" "$ref" | head -8 || true }
  print '```'
}

objects() {
  local base=$1 head=$2 rc=0 arch name verdict
  shift 2
  local -a may=("$@") seen
  print "### The module's objects against my build of the engine's commit"
  print '```'
  for arch in aarch64 arm64ec; do
    local -a b=("$base/$arch-windows"/*.o(N:t)) h=("$head/$arch-windows"/*.o(N:t))
    if (( ! ${#b} )) || [[ ${(j: :)b} != ${(j: :)h} ]]; then
      print "$arch-windows: the base build has ${b:-nothing}, this one ${h:-nothing}"
      rc=1
      continue
    fi
    for name in $b; do
      if cmp -s "$base/$arch-windows/$name" "$head/$arch-windows/$name"; then
        verdict=same
      elif (( ${may[(Ie)$name]} )); then
        verdict="changed, its source is this change's"
        seen+=($name)
      else
        verdict="changed, but nothing this change makes may change it"
        rc=1
      fi
      print "$arch-windows/$name: $verdict"
    done
  done
  for name in $may; do
    (( ${seen[(Ie)$name]} )) || print "$name: this change's source, the same object (a change of comments or of another architecture)"
  done
  if (( rc )); then
    print "result: an object outside this change differs, or the two builds don't have the same objects"
  else
    print "result: no object outside this change differs"
  fi
  print '```'
  return $rc
}

case ${1:-}:$# in
  stage:3 | shape:2 | compare:3) "$@" ;;
  objects:<3->) "$@" ;;
  *) print -u2 "usage: pe.zsh stage <built> <out> | shape <file> | compare <file> <reference> | objects <base> <head> <name>..."; exit 2 ;;
esac
