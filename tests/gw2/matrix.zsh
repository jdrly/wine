#!/bin/zsh
# gw2 (jdrly/gw2-on-mac#40): the test programs under the engine's ntdll.so and mine, in each
# WINE_HEAT_WAKE mode, with tests/gw2/run.zsh.
#
#   matrix.zsh <engine-wine> <my-wine> <exe-dir> <work-dir>
#
# 1. wakes.exe under mine, as WINE_HEAT_EXE with WINE_HEAT_COUNT=1, in each mode (off, one, skip,
#    one,skip), three runs each. The modes run side by side, each in a prefix of its own.
# 1b. evpool.exe under mine, as WINE_HEAT_EXE with WINE_HEAT_COUNT=1, with WINE_HEAT_EVPOOL off, 1
#    and 1,mask, three runs each, side by side (jdrly/gw2-on-mac#42).
# 2. Every program once more, side by side: under the engine's ntdll.so; under mine with no
#    WINE_HEAT_* variable; under mine with the variables set but WINE_HEAT_EXE naming another
#    program. Neither of the last two may write a counters file. The engine skips the programs in
#    $engine_bugs, which show a bug of its file.
# 3. (jdrly/gw2-on-mac#44) mutexall three runs under the engine's ntdll.so, each of which has to
#    fail on the bug it shows, and three under mine, which have to pass. unreg under mine with
#    counting, WINE_HEAT_MSYNC_UNREG unset and set.
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

# mode <mode>: three runs of wakes.exe with counting and WINE_HEAT_WAKE=<mode>. The first keeps test
# 13's minute of producing on one semaphore; the others give it 20 seconds.
mode() {
  local m=$1 n
  for n in 1 2 3; do
    print -r -- "run $n"
    env WINE_HEAT_EXE=wakes.exe WINE_HEAT_COUNT=1 WINE_HEAT_WAKE=$m WINE_HEAT_LOG=$work/log-$m-$n \
      GW2_TEST_PRODUCER_SECONDS=$(( n == 1 ? 60 : 20 )) GW2_TEST_SECONDS=300 \
      "$run" "$mine" "$work/prefix-$m" "$exes/wakes.exe"
  done
}

# programs that fail under the engine's ntdll.so, on a bug of its that mine fixes (block 3)
engine_bugs=(mutexall)

# quiet <name> <wine> <log> [<variable>=<value>...]: every program, then no counters file in <log>
quiet() {
  local name=$1 wine=$2 log=$3 left
  local -a programs=("$exes"/*.exe)
  shift 3
  [[ $wine == "$engine" ]] && programs=(${programs:#*/(${(~j:|:)engine_bugs}).exe})
  mkdir -p "$log"
  env -u WINE_HEAT_COUNT -u WINE_HEAT_WAKE -u WINE_HEAT_EVPOOL -u WINE_HEAT_EXE -u WINE_HEAT_LOG "$@" GW2_TEST_PRODUCER_SECONDS=20 GW2_TEST_SECONDS=300 \
    "$run" "$wine" "$work/prefix-$name" $programs
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

# evpool <mode>: three runs of evpool.exe with counting and WINE_HEAT_EVPOOL=<mode>
evpool() {
  local m=$1 n
  for n in 1 2 3; do
    print -r -- "run $n"
    env WINE_HEAT_EXE=evpool.exe WINE_HEAT_COUNT=1 WINE_HEAT_EVPOOL=$m WINE_HEAT_LOG=$work/log-evpool-$m-$n \
      GW2_TEST_SECONDS=300 "$run" "$mine" "$work/prefix-evpool-$m" "$exes/evpool.exe"
  done
}

for m in off 1 1,mask; do
  mkdir -p "$work"/log-evpool-$m-{1,2,3}
  side "evpool=$m" evpool $m
done
collect

side engine quiet engine "$engine" "$work/log-engine" WINE_HEAT_LOG=$work/log-engine
side unset quiet unset "$mine" "$work/log-unset"
side other quiet other "$mine" "$work/log-other" WINE_HEAT_EXE=other.exe WINE_HEAT_COUNT=1 \
  WINE_HEAT_WAKE=one,skip WINE_HEAT_EVPOOL=1 WINE_HEAT_LOG=$work/log-other
collect

# 3. jdrly/gw2-on-mac#44

# engine_bug <program> <line>: three runs under the engine's ntdll.so, each failing with <line>
engine_bug() {
  local exe=$exes/$1.exe line=$2 out rc=0
  integer n
  out=$(env -u WINE_HEAT_COUNT -u WINE_HEAT_WAKE -u WINE_HEAT_EXE -u WINE_HEAT_LOG GW2_TEST_SECONDS=300 \
    "$run" "$engine" "$work/prefix-bug-$1" "$exe" "$exe" "$exe") || rc=$?
  print -r -- "$out"
  n=${#${(M)${(f)out}:#*$line*}}
  if (( rc && n == 3 )); then
    print "ok    $1 failed 3 of 3 under the engine's ntdll.so, on its bug"
  else
    print "FAIL  $1 under the engine's ntdll.so: $n of 3 runs failed on its bug, the run exited $rc"
    return 1
  fi
}

# unreg <value>: unreg under mine, counting, WINE_HEAT_MSYNC_UNREG=<value> or unset if empty
unreg() {
  local v=$1 log=$work/log-unreg-${1:-unset}
  mkdir -p "$log"
  env -u WINE_HEAT_WAKE -u WINE_HEAT_MSYNC_UNREG WINE_HEAT_EXE=unreg.exe WINE_HEAT_COUNT=1 WINE_HEAT_LOG=$log \
    ${v:+WINE_HEAT_MSYNC_UNREG=$v} GW2_TEST_SECONDS=300 "$run" "$mine" "$work/prefix-unreg-${1:-unset}" "$exes/unreg.exe"
}

side mutexall-engine engine_bug mutexall "another thread took the mutex its owner held"
side mutexall-mine env -u WINE_HEAT_COUNT -u WINE_HEAT_WAKE -u WINE_HEAT_EXE -u WINE_HEAT_LOG GW2_TEST_SECONDS=300 \
  "$run" "$mine" "$work/prefix-mutexall-mine" "$exes/mutexall.exe" "$exes/mutexall.exe" "$exes/mutexall.exe"
side unreg-unset unreg ""
side unreg-1 unreg 1
collect

(( failed == 0 ))
