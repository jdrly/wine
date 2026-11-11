#!/bin/zsh
# gw2 (jdrly/gw2-on-mac#40): run test programs under an engine, in a scratch prefix, without a
# window or Wine's debug output, the way Mach9's smoke test runs its programs (athei/mach9 at
# v0.3.1, bin/bundle-wine.sh: smoke_test, run_limited, smoke_stop).
#
#   run.zsh <wine> <prefix> <exe>...
#
# <wine> is an unpacked engine (the release tarball's wine/). Each program has to print
# "gw2 test <name>: passed" and exit 0 within the time limit, $GW2_TEST_SECONDS (default 120). Afterwards the prefix's wineserver
# is stopped, and no process this script started may be left. Wine rewrites a process's
# arguments, so the processes are found by a variable in their environment, which every child
# and the wineserver inherit. Prints one line per program and the log
# of each that failed; exits 1 if any did.
emulate -L zsh
setopt err_exit pipe_fail no_unset extended_glob

(( $# >= 3 )) || { print -u2 "usage: run.zsh <wine> <prefix> <exe>..."; exit 2 }
wine=${1:A} prefix=${2:A}
shift 2
integer failed=0
export WINEPREFIX=$prefix WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
unset WINEARCH
# only in the environment of what run_limited starts, so ps, grep and this script don't carry it
mark=GW2_TEST_RUN=$$-${wine//[^A-Za-z0-9]/_}

# run_limited <seconds> <log> <command>...: Mach9's, with perl's alarm since macOS has no timeout
run_limited() {
  local secs=$1 log=$2
  shift 2
  perl -e 'alarm shift; exec @ARGV or die "exec: $!\n"' "$secs" env "$mark" "$@" >"$log" 2>&1
}

stop() {
  "$wine/bin/wineserver" -k 2>/dev/null || true
  "$wine/bin/wineserver" -w 2>/dev/null || true
}

mkdir -p "$prefix"
rc=0
# the prefix boot runs wineboot, which took 16 s in Mach9's release run
run_limited 600 "$prefix/boot.log" "$wine/bin/wine" cmd /c exit || rc=$?
if (( rc )); then
  print "FAIL  the prefix boot (exit $rc)"
  cat "$prefix/boot.log"
  stop
  exit 1
fi
print "ok    the prefix boot"

for exe in "$@"; do
  name=${exe:t:r} log=$prefix/${exe:t:r}.log rc=0
  run_limited ${GW2_TEST_SECONDS:-120} "$log" "$wine/bin/wine" "${exe:A}" || rc=$?
  # grep without -q, which would end the pipe early and fail it under pipe_fail
  if (( rc == 0 )) && tr -d '\r' <"$log" | grep -xF "gw2 test $name: passed" >/dev/null; then
    print "ok    $name"
    # what the program said on the way, the counters it read for instance
    tr -d '\r' <"$log" | grep "^gw2 test $name: " | grep -vxF "gw2 test $name: passed" | sed 's/^/      /' || true
  else
    print "FAIL  $name (exit $rc$( (( rc == 142 )) && print ', the time limit' ))"
    sed 's/^/      /' "$log"
    failed+=1
  fi
done

stop
# every process with the mark in its environment (macOS's ps -E prints it after the command)
left=(${(M)${(f)"$(ps -axEww -o pid=,command=)"}:#* $mark( *|)})
if (( ${#left} )); then
  print "FAIL  processes of this run left behind:"
  print -rl -- ${left%% $mark*} | sed 's/^/      /'
  kill -9 ${${left## #}%% *} 2>/dev/null || true
  failed+=1
else
  print "ok    no process of this run left"
fi
(( failed == 0 ))
