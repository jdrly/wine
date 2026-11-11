#!/bin/zsh
# gw2 (jdrly/gw2-on-mac#40): the test programs under the engine's ntdll.so and mine, in each
# WINE_HEAT_WAKE mode, with tests/gw2/run.zsh.
#
#   matrix.zsh <engine-wine> <my-wine> <exe-dir> <work-dir>
#
# 1. wakes.exe under mine, as WINE_HEAT_EXE with WINE_HEAT_COUNT=1, in each mode (off, one, skip,
#    one,skip), three runs each. The modes run side by side, each in a prefix of its own.
# 2. Every program once more, side by side: under the engine's ntdll.so; under mine with no
#    WINE_HEAT_* variable; under mine with the variables set but WINE_HEAT_EXE naming another
#    program. Neither of the last two may write a counters file.
# Prints each run's lines and exits 1 if any failed.
emulate -L zsh
setopt err_exit pipe_fail no_unset extended_glob

(( $# == 4 )) || { print -u2 "usage: matrix.zsh <engine-wine> <my-wine> <exe-dir> <work-dir>"; exit 2 }
engine=${1:A} mine=${2:A} exes=${3:A} work=${4:A}
run=${0:A:h}/run.zsh
mkdir -p "$work"
integer failed=0
pids=() names=()

# side <name> <command>...: start it in the background, its output in $work/<name>.txt
side() {
  local name=$1
  shift
  "$@" >"$work/$name.txt" 2>&1 &
  pids+=($!) names+=("$name")
}

# collect: wait for everything side started, print what each printed
collect() {
  local i rc
  for i in {1..${#pids}}; do
    rc=0
    wait $pids[i] || rc=$?
    print "== $names[i]: $( (( rc )) && print FAIL || print ok)"
    cat "$work/$names[i].txt"
    (( rc == 0 )) || failed+=1
  done
  pids=() names=()
}

# mode <mode>: three runs of wakes.exe with counting and WINE_HEAT_WAKE=<mode>
mode() {
  local m=$1 n
  for n in 1 2 3; do
    print -r -- "run $n"
    env WINE_HEAT_EXE=wakes.exe WINE_HEAT_COUNT=1 WINE_HEAT_WAKE=$m WINE_HEAT_LOG=$work/log-$m-$n \
      GW2_TEST_SECONDS=300 "$run" "$mine" "$work/prefix-$m" "$exes/wakes.exe"
  done
}

# quiet <name> <wine> <log> [<variable>=<value>...]: every program, then no counters file in <log>
quiet() {
  local name=$1 wine=$2 log=$3 left
  shift 3
  mkdir -p "$log"
  env -u WINE_HEAT_COUNT -u WINE_HEAT_WAKE -u WINE_HEAT_EXE -u WINE_HEAT_LOG "$@" GW2_TEST_SECONDS=300 \
    "$run" "$wine" "$work/prefix-$name" "$exes"/*.exe
  left=("$log"/*(N) /tmp/heat-*.log(N))
  if (( ${#left} )); then
    print "FAIL  a counters file was written: $left"
    return 1
  fi
  print "ok    no counters file"
}

for m in off one skip one,skip; do
  mkdir -p "$work"/log-$m-{1,2,3}
  side "wake=$m" mode $m
done
collect

side engine quiet engine "$engine" "$work/log-engine" WINE_HEAT_LOG=$work/log-engine
side unset quiet unset "$mine" "$work/log-unset"
side other quiet other "$mine" "$work/log-other" WINE_HEAT_EXE=other.exe WINE_HEAT_COUNT=1 \
  WINE_HEAT_WAKE=one,skip WINE_HEAT_LOG=$work/log-other
collect

(( failed == 0 ))
