#!/bin/zsh
# gw2 (jdrly/gw2-on-mac#80): the line "switches: <variable> …" of a build's VERSION, every
# <prefix>_* variable the built file reads, sorted, space separated. The app greys a switch's row
# when my file's line doesn't name its variable.
#
#   switches.zsh <prefix> <file> <source>…
#
# The names come from the sources the file was built from: each getenv( "<prefix>_<NAME>" ) whose
# argument is the whole name in one literal. The scan sees no other way of reading one:
# - through a helper or a table (helper( "WINE_HEAT_X" ), { "WINE_HEAT_X", … }) the name is still a
#   string in the file, so the check below finds it missing from the line and fails
# - built from parts at run time ("WINE_HEAT_" and a suffix), the file has the part, and the check
#   fails on that. Split before the whole prefix ("WINE_" and "HEAT_X"), neither sees it: don't.
# Where the check fails, the scan needs to learn the new reader; nobody adds the name to a list.
#
# It fails when the sources name no variable, and when the names aren't exactly the <prefix>_*
# names among the file's strings (strings -a): a getenv the build left out, or a string the scan
# missed. So the line can't name a switch the file lacks, nor leave out one it has.
emulate -L zsh
setopt err_exit pipe_fail no_unset

(( $# >= 3 )) || { print -u2 "usage: switches.zsh <prefix> <file> <source>…"; exit 2 }
prefix=$1 file=$2
shift 2
export LC_ALL=C
die() { print -u2 "::error::switches.zsh $prefix ${file:t}: $*"; exit 1 }

for f in $file "$@"; do [[ -f $f ]] || die "no $f"; done
src=(${(f)"$(grep -ohE "getenv\\( *\"${prefix}_[A-Z0-9_]+\" *\\)" "$@" | grep -oE "${prefix}_[A-Z0-9_]+" | sort -u || true)"})
(( $#src )) || die "no getenv( \"${prefix}_…\" ) in the $# sources"
bin=(${(f)"$(strings -a "$file" | grep -oE "${prefix}_[A-Z0-9_]*" | sort -u || true)"})
if [[ ${(j: :)src} != "${(j: :)bin}" ]]; then
  only_src=(${src:|bin}) only_bin=(${bin:|src})
  die "only in the sources: ${only_src[*]:-none}; only in the file's strings: ${only_bin[*]:-none}"
fi
print "switches: ${(j: :)src}"
