#!/bin/sh
# fgres emulation harness: fake /proc tree + fake cgroup.procs, no phone input.
# Usage: emulate.sh [path-to-fgres-binary]   (host or android build)
BIN=${1:-/data/local/tmp/fgres}
T=$(mktemp -d "${TMPDIR:-/tmp}/fgtest.XXXXXX")
trap 'kill $DPID 2>/dev/null; rm -rf $T' EXIT
mkdir -p "$T/proc"

# --- fake proc tree: pid -> uid (chown), adj, cmdline ---
mkpid() { # pid uid adj cmdline-name
  mkdir "$T/proc/$1"
  chown "$2" "$T/proc/$1"
  printf '%s\n' "$3" > "$T/proc/$1/oom_score_adj"
  printf '%s\0' "$4" > "$T/proc/$1/cmdline"
}
mkpid 151 1000   -900 system_server
mkpid 100 10151    0 com.google.android.apps.nexuslauncher
mkpid 200 10111    0 com.google.android.googlequicksearchbox:googleapp
mkpid 300 10136    0 com.android.chrome
mkpid 301 10136    0 com.android.chrome:sandboxed_process0
mkpid 400 90100    0 com.android.chrome  # isolated: per-proc uid 9xxxx
mkpid 672 10131  200 com.google.android.inputmethod.latin
mkpid 800 10190    0 /system/bin/sh   # terminal-app subprocess (real bug: uid 10190 shell at adj 0)

mkdir -p "$T/watch"
payload() { for p in "$@"; do printf '%s\n' "$p"; done > "$T/watch/cgroup.procs"; }

payload 151 100 200 672                      # initial: launcher fg, googleapp service along
FGRES_STDOUT=1 "$BIN" "$T/watch/cgroup.procs" "$T/proc" > "$T/out.log" 2>/dev/null &
DPID=$!
sleep 0.3

payload 151 100 200 672                      # T1a: identical rewrite -> hash dedupe
payload 151 100 200 672                      # T1b: again
sleep 0.3
payload 151 300 400 672                      # T2: chrome fg + isolated 400 (must be ignored)
sleep 0.3
payload 151 300 301 400 672                  # T3: content changed, same fg -> result dedupe
sleep 0.3
payload 151 100 200 300 672                  # T4: 3 real uids at adj 0 -> bail, no emit
sleep 0.3
payload 151 100 300 672                      # T5: true 2-activity ambiguity -> None
sleep 0.3
payload 151 300 200 672                      # T6: chrome + googleapp(:) -> chrome wins tiebreak
sleep 0.3
payload 151 100 800 672                      # T7: launcher vs sh(/) uid -> launcher wins tiebreak
sleep 0.3
kill $DPID 2>/dev/null || true
sleep 0.2

echo "--- emissions ---"
cat "$T/out.log"
echo "--- expect: 100(T0) 300(T2) [100 T4 transient] 300(T6) 100(T7); T3/T5 silent ---"
