# Changelog

## [1.0.0] - 2026-09-28

Initial release.

- Event-driven foreground resolution: watches
  `/dev/cpuset/top-app/cgroup.procs` with inotify, zero CPU between events.
- Four resolution gates: app_id, thread count (`<pid>/task` single fstat),
  literal `oom_score_adj == 0`, and cmdline identity vetting (shells,
  `:service` subprocesses, and bare uid-1000 procs never emit).
- Package names from the winner's `cmdline` argv[0] — no package database,
  no reload forks.
- Best-effort stdout (`SIGPIPE` ignored, non-blocking); logcat always.
- Single C11 file (~550 lines), stripped `arm64` binary (~10 KiB),
  `make test` + NDK CI.
