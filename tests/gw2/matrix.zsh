#!/bin/zsh
# gw2 (jdrly/gw2-on-mac#40): the test programs under the engine's ntdll.so and mine, in each
# WINE_HEAT_WAKE mode, with tests/gw2/run.zsh.
#
#   matrix.zsh <engine-wine> <my-wine> <exe-dir> <work-dir> [<win32u-wine>]
#
# <win32u-wine> is the engine with my ntdll.so and my win32u.so, when the build ships one.
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
# 4. (jdrly/gw2-on-mac#45) spin.exe under mine, as WINE_HEAT_EXE with WINE_HEAT_COUNT=1, with
#    WINE_HEAT_SPIN unset and in three settings, three runs each, one setting at a time: the app's string
#    (lock and event at :game), sem alone at its default scope with WINE_HEAT_WAKE=one,skip (the
#    spin against the wake skip's sleeper count), and every kind on with a scope of its own.
# 5. (jdrly/gw2-on-mac#43) prof.exe under mine as WINE_HEAT_EXE with WINE_HEAT_PROF=1 and
#    FEX_PROFILESTATS=1, once, 8 seconds. Block 2 runs it for a second with the sampler unset and set
#    for another program, where no file of the sampler's may appear either.
# 6. (jdrly/gw2-on-mac#60) requests three runs under mine with counting. Beside it,
#    (jdrly/gw2-on-mac#99) timers three runs under mine with counting.
# 7. (jdrly/gw2-on-mac#61) with <win32u-wine>: setcursor, then requests and peekyield with
#    counting, under the engine's win32u.so with WINE_WIN32U_SETCURSOR=shm and
#    WINE_WIN32U_PEEK_YIELD=off, and under mine with neither, with only the first and with only the
#    second. Only mine leaves the repeated cursor's requests out (GW2_TEST_SETCURSOR_SKIPS=1) or
#    the empty peek's yield (GW2_TEST_PEEK_NO_YIELD=1), each with its own switch. Block 2 ran
#    setcursor and peekyield under the engine's win32u.so with neither set.
# 8. (jdrly/gw2-on-mac#101) timeout three runs under the engine's ntdll.so and three under mine.
# Prints each run's lines and exits 1 if any failed.
emulate -L zsh
setopt err_exit pipe_fail no_unset extended_glob

(( $# == 4 || $# == 5 )) || { print -u2 "usage: matrix.zsh <engine-wine> <my-wine> <exe-dir> <work-dir> [<win32u-wine>]"; exit 2 }
engine=${1:A} mine=${2:A} exes=${3:A} work=${4:A} win32u=${5:+${5:A}}
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
  env -u WINE_HEAT_COUNT -u WINE_HEAT_WAKE -u WINE_HEAT_EVPOOL -u WINE_HEAT_SPIN -u WINE_HEAT_PROF -u WINE_HEAT_EXE -u WINE_HEAT_LOG \
    -u WINE_WIN32U_SETCURSOR -u WINE_WIN32U_PEEK_YIELD -u GW2_TEST_SETCURSOR_SKIPS -u GW2_TEST_PEEK_NO_YIELD "$@" \
    GW2_TEST_PRODUCER_SECONDS=20 GW2_TEST_SECONDS=300 \
    "$run" "$wine" "$work/prefix-$name" $programs
  left=("$log"/*(N) /tmp/heat-*(N))
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
  WINE_HEAT_WAKE=one,skip WINE_HEAT_EVPOOL=1 WINE_HEAT_SPIN=lock=20,event=30,sem=5 WINE_HEAT_PROF=1 WINE_HEAT_LOG=$work/log-other
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

# 4. jdrly/gw2-on-mac#45

# spin <name> <setting> [<wake>]: three runs of spin.exe with counting, WINE_HEAT_SPIN=<setting>
# (unset if empty) and WINE_HEAT_WAKE=<wake> (unset if not given)
spin() {
  local name=$1 v=$2 w=${3:-} n
  for n in 1 2 3; do
    print -r -- "run $n"
    mkdir -p "$work/log-spin-$name-$n"
    env -u WINE_HEAT_WAKE -u WINE_HEAT_SPIN WINE_HEAT_EXE=spin.exe WINE_HEAT_COUNT=1 \
      WINE_HEAT_LOG=$work/log-spin-$name-$n ${v:+WINE_HEAT_SPIN=$v} ${w:+WINE_HEAT_WAKE=$w} GW2_TEST_SECONDS=300 \
      "$run" "$mine" "$work/prefix-spin-$name" "$exes/spin.exe"
  done
}

# one mode at a time: a spin hits only when the thread it waits for runs during it, and four
# programs side by side, each with two busy threads, leave that to chance on 3 CPUs
side spin-off spin off ""
collect
side spin-app spin app "lock=20:game,event=30:game"
collect
side spin-sem spin sem "sem=5" one,skip
collect
side spin-scopes spin scopes "lock=20:all,event=30:not=2,sem=20:game"
collect

# 5. jdrly/gw2-on-mac#43

mkdir -p "$work/log-prof"
side prof env -u WINE_HEAT_COUNT -u WINE_HEAT_WAKE -u WINE_HEAT_EVPOOL -u WINE_HEAT_SPIN WINE_HEAT_EXE=prof.exe \
  WINE_HEAT_PROF=1 FEX_PROFILESTATS=1 WINE_HEAT_LOG=$work/log-prof GW2_TEST_SECONDS=300 \
  "$run" "$mine" "$work/prefix-prof" "$exes/prof.exe"
collect

# 6. jdrly/gw2-on-mac#60: requests three runs under mine with counting, each with a log folder of
# its own, so each finds its own heat-requests-<pid>.log. After block 5, not beside it: prof
# asserts a sample rate and an attribution ratio. Block 2 ran it without counting.
requests() {
  local n log
  for n in 1 2 3; do
    print -r -- "run $n"
    log=$work/log-requests-$n
    mkdir -p "$log"
    env -u WINE_HEAT_WAKE -u WINE_HEAT_MSYNC_UNREG -u WINE_HEAT_EVPOOL -u WINE_HEAT_SPIN -u WINE_HEAT_PROF \
      -u WINE_WIN32U_SETCURSOR -u WINE_WIN32U_PEEK_YIELD -u GW2_TEST_SETCURSOR_SKIPS -u GW2_TEST_PEEK_NO_YIELD \
      WINE_HEAT_EXE=requests.exe WINE_HEAT_COUNT=1 WINE_HEAT_LOG=$log \
      GW2_TEST_SECONDS=300 "$run" "$mine" "$work/prefix-requests" "$exes/requests.exe"
  done
}

# jdrly/gw2-on-mac#99: timers beside it. It checks counts only, no rates. Block 2 ran it without counting.
timers() {
  local n log
  for n in 1 2 3; do
    print -r -- "run $n"
    log=$work/log-timers-$n
    mkdir -p "$log"
    env -u WINE_HEAT_WAKE -u WINE_HEAT_MSYNC_UNREG -u WINE_HEAT_EVPOOL -u WINE_HEAT_SPIN -u WINE_HEAT_PROF \
      -u WINE_WIN32U_SETCURSOR -u WINE_WIN32U_PEEK_YIELD -u GW2_TEST_SETCURSOR_SKIPS -u GW2_TEST_PEEK_NO_YIELD \
      WINE_HEAT_EXE=timers.exe WINE_HEAT_COUNT=1 WINE_HEAT_LOG=$log \
      GW2_TEST_SECONDS=300 "$run" "$mine" "$work/prefix-timers" "$exes/timers.exe"
  done
}

side requests requests
side timers timers
collect

# 7. jdrly/gw2-on-mac#61

# win32u_run <name> <wine> [<variable>=<value>...]: setcursor once, then requests and peekyield with
# counting, each with a log folder of its own, under <wine> with the variables
win32u_run() {
  local name=$1 wine=$2 exe
  shift 2
  for exe in setcursor requests peekyield; do
    mkdir -p "$work/log-$exe-$name"
    env -u WINE_HEAT_WAKE -u WINE_HEAT_MSYNC_UNREG -u WINE_HEAT_EVPOOL -u WINE_HEAT_SPIN -u WINE_HEAT_PROF \
      -u WINE_HEAT_EXE -u WINE_HEAT_COUNT -u WINE_WIN32U_SETCURSOR -u WINE_WIN32U_PEEK_YIELD \
      -u GW2_TEST_SETCURSOR_SKIPS -u GW2_TEST_PEEK_NO_YIELD "$@" \
      ${${exe:#setcursor}:+WINE_HEAT_EXE=$exe.exe} ${${exe:#setcursor}:+WINE_HEAT_COUNT=1} \
      WINE_HEAT_LOG=$work/log-$exe-$name GW2_TEST_SECONDS=300 \
      "$run" "$wine" "$work/prefix-win32u-$name" "$exes/$exe.exe" || return 1
  done
}

if [[ -n $win32u ]]; then
  side win32u-engine-set win32u_run engine-set "$mine" WINE_WIN32U_SETCURSOR=shm WINE_WIN32U_PEEK_YIELD=off
  side win32u-mine-unset win32u_run mine-unset "$win32u"
  side win32u-mine-shm win32u_run mine-shm "$win32u" WINE_WIN32U_SETCURSOR=shm GW2_TEST_SETCURSOR_SKIPS=1
  side win32u-mine-off win32u_run mine-off "$win32u" WINE_WIN32U_PEEK_YIELD=off GW2_TEST_PEEK_NO_YIELD=1
  collect
else
  print "== win32u: no win32u.so of mine in this build, so block 7 has nothing to run"
fi

# 8. jdrly/gw2-on-mac#101: timeout three runs under the engine's ntdll.so and three under mine, with
# no WINE_HEAT_* variable. The two side by side only: its waits have to end no earlier than 45 ms
# into 50, and the shortest of each 20 before 150.
side timeout-engine env -u WINE_HEAT_COUNT -u WINE_HEAT_EXE -u WINE_HEAT_LOG GW2_TEST_SECONDS=300 \
  "$run" "$engine" "$work/prefix-timeout-engine" "$exes/timeout.exe" "$exes/timeout.exe" "$exes/timeout.exe"
side timeout-mine env -u WINE_HEAT_COUNT -u WINE_HEAT_EXE -u WINE_HEAT_LOG GW2_TEST_SECONDS=300 \
  "$run" "$mine" "$work/prefix-timeout-mine" "$exes/timeout.exe" "$exes/timeout.exe" "$exes/timeout.exe"
collect

(( failed == 0 ))
