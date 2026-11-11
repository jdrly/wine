#!/bin/zsh
# gw2 (jdrly/gw2-on-mac#40): run test programs under an engine, in a scratch prefix, without a
# window or Wine's debug output, the way Mach9's smoke test runs its programs (athei/mach9 at
# v0.3.1, bin/bundle-wine.sh: smoke_test, run_limited, smoke_stop).
#
#   run.zsh <wine> <prefix> <exe>...
#
# <wine> is an unpacked engine (the release tarball's wine/). Each program has to print
# "gw2 test <name>: passed" and exit 0 within the time limit. Afterwards the prefix's wineserver
# is stopped, and no process of the engine may be left. Prints one line per program and the log
# of each that failed; exits 1 if any did.
emulate -L zsh
setopt err_exit pipe_fail no_unset

(( $# >= 3 )) || { print -u2 "usage: run.zsh <wine> <prefix> <exe>..."; exit 2 }
wine=${1:A} prefix=${2:A}
shift 2
integer failed=0
export WINEPREFIX=$prefix WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
unset WINEARCH

# run_limited <seconds> <log> <command>...: Mach9's, with perl's alarm since macOS has no timeout
run_limited() {
  local secs=$1 log=$2
  shift 2
  perl -e 'alarm shift; exec @ARGV or die "exec: $!\n"' "$secs" "$@" >"$log" 2>&1
}

stop() {
  "$wine/bin/wineserver" -k 2>/dev/null || true
  "$wine/bin/wineserver" -w 2>/dev/null || true
}

mkdir -p "$prefix"
status=0
# the prefix boot runs wineboot, which took 16 s in Mach9's release run
run_limited 600 "$prefix/boot.log" "$wine/bin/wine" cmd /c exit || status=$?
if (( status )); then
  print "FAIL  the prefix boot (exit $status)"
  cat "$prefix/boot.log"
  stop
  exit 1
fi
print "ok    the prefix boot"

for exe in "$@"; do
  name=${exe:t:r} log=$prefix/${exe:t:r}.log status=0
  run_limited 120 "$log" "$wine/bin/wine" "${exe:A}" || status=$?
  # grep without -q, which would end the pipe early and fail it under pipe_fail
  if (( status == 0 )) && tr -d '\r' <"$log" | grep -xF "gw2 test $name: passed" >/dev/null; then
    print "ok    $name"
  else
    print "FAIL  $name (exit $status$( (( status == 142 )) && print ', the time limit' ))"
    sed 's/^/      /' "$log"
    failed+=1
  fi
done

stop
left=$(pgrep -fl -- "$wine/" || true)
if [[ -n $left ]]; then
  print "FAIL  processes of the engine left behind:"
  print -r -- "$left" | sed 's/^/      /'
  pkill -9 -f -- "$wine/" || true
  failed+=1
else
  print "ok    no process of the engine left"
fi
(( failed == 0 ))
