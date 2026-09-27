# fgres

Event-driven foreground resolution for Android, in one C11 file (~550 lines).

Watches `/dev/cpuset/top-app/cgroup.procs` with inotify and emits one line
per foreground change:

```
fg pid=31274 uid=10136 pkg=com.android.chrome
```

## Resolution

Each cgroup member passes four gates, cheapest first, all from a single
`fstat` plus small procfs reads — no heap, no parsing libraries:

1. **app_id** (`uid % 100000`): platform uid 1000 or user apps 10000+;
   isolated and SDK-sandbox ranges excluded.
2. **threads**: one `fstat` on `/proc/<pid>/task` yields uid and thread
   count together — a spawned shell runs 1–3 threads, a Zygote app 20+;
   at most 8 threads never qualifies.
3. **adj**: `oom_score_adj` must read literally `0\n` (the kernel's
   canonical formatting — compared, not parsed).
4. **identity** (emittable shapes only): winner's `cmdline` argv[0] must
   carry no `:`/`/` (services and exec'd shells lose), and uid-1000
   processes must be dotted APK names (never bare system procs). Package
   names come from that same argv[0], so there is no package database to
   keep fresh — installs resolve on their first transition with no reload.

Same-uid pids collapse (lowest wins); two vetted uids at adj 0 is
split-screen/transient → silent; identical payloads dedupe before any of
this runs. A transient (adj unsettled, `<pre-initialized>`) never latches,
so the next event retries it.

## Output sinks

Every emission goes to the **log** always: logcat on Android (tag `fgres`),
stderr on the host. **stdout** only when a listener is attached — either
`isatty(1)` (you ran it in a terminal) or `FGRES_STDOUT=1` (a pipe consumer,
e.g. another daemon reading fgres line by line). stdout is best-effort
(`SIGPIPE` ignored, non-blocking): a dead or full consumer can neither
stall nor kill the loop; logcat still gets the line.

```sh
./fgres                              # terminal: log + stdout
FGRES_STDOUT=1 ./fgres | while ...   # pipe listener contract
```

## Build

```sh
make            # unit tests + host binary + android binary
make test       # target/fgres_test (assert-based, no framework)
```

Needs `clang` with `lld` (the `-flto` link). Android cross-build needs
`aarch64-linux-android24-clang` on PATH; override with `ANDROID_CC=...`.
Output in `target/`: `fgres` (host), `fgres_test`, `fgres-android`.

## Test harness

`scripts/emulate.sh` builds a fake /proc tree + fake cgroup.procs and drives
the binary through transitions covering dedupe, sandbox ignore,
split-screen bail, and `:`-subprocess / `/`-shell tiebreaks, with no phone
input:

```sh
scripts/emulate.sh target/fgres          # or /data/local/tmp/fgres-android
```

## Notes

- The 3-arg form `fgres <watch-path> <proc-root>` is a test hook for the
  emulation harness; production takes no arguments.
- `cgroup.procs` is re-read with open+read+close on every event: cgroup v1
  caches the pidlist per open fd for ~1s, so a persistent fd + pread() would
  miss mid-burst transitions.
- Both `cgroup.procs` and its sibling `tasks` are watched: some OEM AMS
  paths migrate via `tasks`, which notifies only the `tasks` inode.

## License

GPL-3.0-only — see [LICENSE](LICENSE).
