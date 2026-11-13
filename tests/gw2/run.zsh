#!/bin/zsh
# gw2 (jdrly/gw2-on-mac#40): run test programs under an engine, in a scratch prefix, without a
# window or Wine's debug output, the way Mach9's smoke test runs its programs (athei/mach9 at
# v0.3.1, bin/bundle-wine.sh: smoke_test, run_limited, smoke_stop).
#
#   run.zsh <wine> <prefix> <exe>...
#
# <wine> is an unpacked engine (the release tarball's wine/). WINEDEBUG is -all, or
# $GW2_TEST_WINEDEBUG when set; each program's log stays in <prefix>/<name>.log. Each program has to print
# "gw2 test <name>: passed" and exit 0 within the time limit, $GW2_TEST_SECONDS (default 120). Afterwards the prefix's wineserver
# is stopped, and no process this script started may be left (jdrly/gw2-on-mac#128): none in the
# process groups of the commands it ran, and none with this run's variable in its environment.
# Each command leads a group of its own, which every process it starts stays in, unless Wine
# gives one a console or group of its own (setsid in ntdll's spawn_process, and the wineserver),
# and the wineserver ends those. The variable alone doesn't do: Wine rewrites what ps reads, and
# on GitHub's runner ps -E showed it for none of Wine's processes (jdrly/FEX run 43297396698).
# The groups also keep the runs matrix.zsh starts side by side under one engine apart. Ends what
# is left by pid. Prints one line per program and the log of each that failed; exits 1 if any did.
emulate -L zsh
setopt err_exit pipe_fail no_unset extended_glob

(( $# >= 3 )) || { print -u2 "usage: run.zsh <wine> <prefix> <exe>..."; exit 2 }
wine=${1:A} prefix=${2:A}
shift 2
integer failed=0
export WINEPREFIX=$prefix WINEDEBUG=${GW2_TEST_WINEDEBUG:--all} WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
unset WINEARCH
# only in the environment of what run_limited starts, so ps, grep and this script don't carry it
mark=GW2_TEST_RUN=$$-${wine//[^A-Za-z0-9]/_}

# the process groups run_limited started, one per command (zsh's $groups is taken)
pgids=()

# run_limited <seconds> <log> <command>...: Mach9's, with perl's alarm since macOS has no timeout,
# and the command in a process group of its own, whose id is its pid
run_limited() {
  local secs=$1 log=$2 rc=0
  shift 2
  perl -e 'setpgrp or die "setpgrp: $!\n"; alarm shift; exec @ARGV or die "exec: $!\n"' \
    "$secs" env "$mark" "$@" >"$log" 2>&1 &
  pgids+=($!)
  wait $! || rc=$?
  return $rc
}

# the pids of this run's processes: the live ones in its groups, and the ones with the mark in
# their environment (macOS's ps -E prints it after the command). A zombie (Z) or a process on its
# way out (E) is ending already. Wine starts services.exe, what it runs, explorer and conhost
# detached, each in a session of its own, so in a run that leaves nothing the only process the
# wineserver knew that can still be in a group is a child a program waited for (wakes.c's). The
# wineserver waits until that child's pid is gone, unless the child stalls half a second between
# closing its socket and exit(): then the wineserver kills it and stops waiting, and the check would
# count it.
ours() {
  local line f
  for line in ${(f)"$(ps -ax -o pid=,pgid=,stat=)"}; do
    f=(${=line})
    if (( ${pgids[(Ie)$f[2]]} )) && [[ $f[3] != (Z*|*E*) ]]; then
      print -r -- $f[1]
    fi
  done
  print -l -- ${${${(M)${(f)"$(ps -axEww -o pid=,command=)"}:#* $mark( *|)}## #}%% *}
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
left=(${(u)${(f)"$(ours)"}})
if (( ${#left} )); then
  print "FAIL  processes of this run left behind:"
  ps -ww -o pid=,command= -p ${(j:,:)left} | sed 's/^/      /' || true
  kill -9 $left 2>/dev/null || true
  failed+=1
else
  print "ok    no process of this run left"
fi
(( failed == 0 ))
