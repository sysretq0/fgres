/*
 * fgres: event-driven foreground resolution for Android.
 * Watches /dev/cpuset/top-app/cgroup.procs with inotify; filters members by
 * app_id (uid % 100000: user apps 10000+, platform-signed uid 1000),
 * thread count (one fstatat on <pid>/task: shells run 1-3, Zygote apps 20+),
 * and oom_score_adj == 0 (kernel formats it canonically as "0\n": a literal
 * 2-byte compare, no parsing); emits "fg pid= uid= pkg=" on change to the log
 * (logcat, always) and to stdout, but stdout only when a listener is
 * attached (isatty) or FGRES_STDOUT=1 forces it for a pipe harness.
 * Package names come from the winner's /proc/<pid>/cmdline argv[0] (a vetted
 * top-level app process always carries its bare package name).
 *
 * Zero-allocation hot path: all parsing/formatting is stack-only ASCII;
 * cgroup.procs is re-read per event with open+read+close (a persistent fd +
 * pread() would serve the kernel's cached pidlist snapshot for ~1s on
 * cgroup v1, missing mid-burst transitions).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE 1 /* inotify_*, O_CLOEXEC under -std=c11 (no-op on bionic) */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#define CPATH "/dev/cpuset/top-app/cgroup.procs"
#define TAG "fgres"

/* Android isolated-process app_id range: one unique app_id per sandbox proc
 * (WebView renderers etc). A sandbox at adj 0 implies its host app is fg;
 * the host carries the emission. Sandboxes are never candidates, never veto. */
#define ISOLATED_LO 90000u
#define ISOLATED_HI 99999u

/* SDK sandbox app_id range (Android 13+): per-app unique, same treatment. */
#define SDK_SANDBOX_LO 20000u
#define SDK_SANDBOX_HI 29999u

/* Fixed payload buffer: ~170 pids (1024B / 6B line). A read that fills the
 * buffer exactly is treated as suspect truncation and skipped, not parsed. */
#define BUFSZ 1024
#define PATHMAX 64 /* argv path override cap (watch path and /proc root) */
#define PKGMAX 255 /* max Android package name length */

/* Debug build only (-DFGRES_DEBUG): branch trace on the event loop. */
#ifdef FGRES_DEBUG
#define DBG(s) log_print(3, s, sizeof(s) - 1)
#else
#define DBG(s) ((void)0)
#endif

/* Resolved foreground: pid + uid. adj is invariant 0 for candidates.
 * pid==0 is the "none" sentinel; PIDs are always > 0. */
typedef struct {
    uint32_t pid;
    uint32_t uid;
} Fg;

/* Single-pass digit scan of cgroup.procs payloads. */
static size_t scan_pids(const uint8_t *p, const uint8_t *end, uint32_t *out, size_t cap) {
    size_t cnt = 0;
    while (p < end) {
        uint32_t pid = 0;
        while (p < end && *p >= '0' && *p <= '9')
            pid = pid * 10u + (uint32_t)(*p++ - '0');
        while (p < end && (*p < '0' || *p > '9'))
            p++;
        if (pid && cnt < cap)
            out[cnt++] = pid;
    }
    return cnt;
}

#ifndef FGRES_TEST
/* Sink policy: the log always gets every line (logcat on Android; stderr on
 * the host, which has no logcat). stdout is the listener surface — written
 * only when one is attached: isatty(1) or FGRES_STDOUT=1 (the env override
 * for pipe harnesses, what emulate.sh uses). Cached once at startup;
 * isatty per event would be a syscall per event. */
static bool stdout_listener;

static void log_print(int prio, const char *msg, size_t len) {
    /* write() not fwrite(): stdio is block-buffered on pipes and would sit
     * on emissions the harness reads live. Single writev: one syscall for
     * msg + newline on every path (die/DBG/emit alike). */
    if (stdout_listener) {
        struct iovec iv[2] = {{(void *)msg, len}, {(void *)"\n", 1}};
        ssize_t w = writev(STDOUT_FILENO, iv, 2);
        (void)w;
    }
#ifdef __ANDROID__
    __android_log_print(prio, TAG, "%.*s", (int)len, msg);
#else
    char line[384]; /* fits "fgres[prio=N]: " + 292-byte pkg emission */
    int n = snprintf(line, sizeof(line), "fgres[prio=%d]: %.*s\n", prio, (int)len, msg);
    if (n > 0) {
        ssize_t w = write(STDERR_FILENO, line, (size_t)n);
        (void)w;
    }
#endif
}

static void die(const char *msg) {
    log_print(5, msg, strlen(msg)); /* ANDROID_LOG_ERROR */
    _exit(1);
}
#endif

/* ---- stack-only ASCII output (no snprintf in the per-PID path) ---- */

/* Write decimal v into buf at i, prefixed by a literal (len known at
 * compile time). Returns new i. */
#define write_u32(buf, i, lit, v) write_u32_((buf), (i), (lit), sizeof(lit) - 1u, (v))
static size_t write_u32_(char *buf, size_t i, const char *lit, size_t litlen, uint32_t v) {
    memcpy(buf + i, lit, litlen);
    i += litlen;
    char tmp[10];
    int j = 0;
    do {
        tmp[j++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (j > 0)
        buf[i++] = tmp[--j];
    return i;
}

/* ---- path builders / procfs readers ---- */

/* Format "<pid>/<suffix>" (relative to the open /proc dirfd) into buf.
 * Returns byte count (no NUL). */
static size_t fmt_proc_path(char *buf, uint32_t pid, const char *suffix) {
    size_t i = write_u32_(buf, 0, "", 0, pid);
    buf[i++] = '/';
    size_t sl = strlen(suffix);
    memcpy(buf + i, suffix, sl);
    return i + sl;
}

#ifndef FGRES_TEST
/* fstatat <procfd>/<pid>/task: uid of the dir (kernel sets it to the process
 * uid) plus the thread count (task-dir st_nlink == threads + 2; a Zygote app
 * carries Binder/HeapTaskDaemon/RenderThread, a spawned sh sits at 1).
 * Returns uid, 0 if gone; *nthread gets the count (0 if gone). */
#define THREADMIN 8u /* elect only processes with >8 threads; shells run 1-3 */
static uint32_t fstat_proc(int procfd, uint32_t pid, unsigned *nthread) {
    char path[16]; /* 10 digits + "/task" + NUL */
    size_t n = fmt_proc_path(path, pid, "task");
    path[n] = '\0';
    struct stat st;
    if (fstatat(procfd, path, &st, 0) != 0) {
        *nthread = 0;
        return 0;
    }
    *nthread = st.st_nlink > 2 ? (unsigned)(st.st_nlink - 2) : 0;
    return (uint32_t)st.st_uid;
}

/* Read absolute file path into buf. Returns bytes read, 0 on failure. */
static size_t read_file(const char *path, uint8_t *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    ssize_t r = read(fd, buf, cap);
    close(fd);
    return r > 0 ? (size_t)r : 0;
}

static size_t read_proc_file(int procfd, uint32_t pid, const char *file, uint8_t *buf,
                             size_t cap) {
    char path[32];
    size_t n = fmt_proc_path(path, pid, file);
    path[n] = '\0';
    int fd = openat(procfd, path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    ssize_t r = read(fd, buf, cap);
    close(fd);
    return r > 0 ? (size_t)r : 0;
}

/* ---- emit-path vetting + package name: winner's cmdline argv[0] ---- */

/* Vet the winner and copy its package name in a single cmdline read -> pkg
 * length into out (>= PKGMAX+1 bytes). A vetted top-level app process always
 * carries its bare package name as argv[0]. Returns 0 (no emit, no latch) on
 * shells/services/bare procs (same rule as the tiebreak) and on transient
 * empty/'<pre-initialized>' cmdlines. */
static size_t vet_pkg(int procfd, uint32_t pid, char *out) {
    uint8_t cbuf[PKGMAX];
    size_t n = read_proc_file(procfd, pid, "cmdline", cbuf, sizeof(cbuf));
    size_t end = 0;
    while (end < n && cbuf[end] != 0)
        end++;
    if (end == 0 || cbuf[0] == '<')
        return 0; /* empty or <pre-initialized>: transient */
    bool dot = false;
    for (size_t i = 0; i < end; i++) {
        if (cbuf[i] == ':' || cbuf[i] == '/')
            return 0; /* service/shell: never emit */
        if (cbuf[i] < 0x20 || cbuf[i] == 0x7f)
            return 0; /* control bytes: argv is userspace-writable, never
                       * let it forge log lines */
        if (cbuf[i] == '.')
            dot = true;
    }
    if (!dot)
        return 0; /* dotted APK procs only, whatever the uid */
    memcpy(out, cbuf, end);
    out[end] = '\0';
    return end;
}

/* Tiebreak predicate via the emit vetter: a second cmdline read is cheaper
 * than a divergent copy. Note the deliberate flip vs the old inline scan:
 * an empty (reaped-transient) cmdline now counts as not-app, so a live
 * candidate paired with a transient wins immediately instead of waiting a
 * payload round. */
static bool cmdline_not_app(int procfd, uint32_t pid) {
    char tmp[PKGMAX + 1];
    return vet_pkg(procfd, pid, tmp) == 0;
}
#endif

#ifndef FGRES_TEST
/* Shared candidate gate: fstat uid+threads, app_id allowlist, oom adj 0.
 * Returns the uid, or 0 on reject. */
static uint32_t gated_uid(int procfd, uint32_t pid) {
    /* fstatat <pid>/task -> uid + thread count. Cheap, no read of file
     * content. A spawned shell/pty runs 1-3 threads; a Zygote app
     * carries 20+. Rejected outright (never merged). */
    unsigned nthread;
    uint32_t uid = fstat_proc(procfd, pid, &nthread);
    if (uid == 0 || nthread <= THREADMIN)
        return 0;
    /* app_id normalizes across users/work profiles (uid = user*100000 + app_id) */
    uint32_t app_id = uid % 100000u;
    bool keep = app_id == 1000u || (app_id >= 10000u &&
                                    !(SDK_SANDBOX_LO <= app_id && app_id <= SDK_SANDBOX_HI) &&
                                    !(ISOLATED_LO <= app_id && app_id <= ISOLATED_HI));
    if (!keep)
        return 0;
    /* oom gate: the kernel formats adj 0 canonically as "0\n" — the only
     * value accepted, so a literal 2-byte compare replaces parsing. */
    uint8_t adj_buf[2];
    size_t n = read_proc_file(procfd, pid, "oom_score_adj", adj_buf, sizeof(adj_buf));
    if (n != 2 || adj_buf[0] != '0' || adj_buf[1] != '\n')
        return 0;
    return uid;
}

/* Runner-up retry for the emit path: if the winner fails vetting (e.g. a
 * stale ':service' subprocess outliving its restarted main process), try
 * the next gated pid of the same uid from the same payload. Returns 0
 * when no runner-up vets clean. */
static uint32_t retry_same_uid(const uint8_t *payload, size_t len, int procfd,
                               uint32_t uid, uint32_t after_pid,
                               char *out) {
    uint32_t pids[BUFSZ / 2];
    size_t npid = scan_pids(payload, payload + len, pids, sizeof(pids) / sizeof(pids[0]));
    for (size_t pi = 0; pi < npid; pi++) {
        uint32_t pid = pids[pi];
        if (pid <= after_pid)
            continue;
        if (gated_uid(procfd, pid) != uid)
            continue;
        if (vet_pkg(procfd, pid, out) != 0)
            return pid;
    }
    return 0;
}
#endif

/*
 * Resolve the foreground app from a top-app payload.
 *
 * Candidate = app_id gate AND thread gate (one fstatat on <pid>/task: uid
 * plus threads; shells run 1-3, Zygote apps 20+) AND oom_score_adj == 0.
 * Sandbox processes (isolated / SDK-sandbox app_ids) carry per-proc unique uids
 * and are excluded entirely. Same-uid candidates collapse (lowest pid wins as
 * representative). A lone candidate returns unvetted; the emit path vets +
 * names it in one cmdline read (vet_pkg). 2 distinct real uids at adj 0 happens legitimately (home:
 * launcher + app:service; split-screen: two activities): drop ':'-bearing
 * process names via cmdline (service subprocesses are never emitted
 * themselves; note an activity *can* legally live in an
 * android:process=":x" subprocess, which this rule then misses — accepted
 * limitation, the common case is unambiguous);
 * exactly one survivor wins, otherwise none. 3rd distinct uid -> bail now.
 * Returns pid==0 for none.
 */
#ifndef FGRES_TEST
static Fg resolve(const uint8_t *payload, size_t len, int procfd) {
    Fg cand = {0, 0}, second = {0, 0};
    uint32_t pids[BUFSZ / 2]; /* "<pid>\n" >= 2 bytes/line */
    size_t npid = scan_pids(payload, payload + len, pids, sizeof(pids) / sizeof(pids[0]));
    for (size_t pi = 0; pi < npid; pi++) {
        uint32_t pid = pids[pi];

        uint32_t uid = gated_uid(procfd, pid);
        if (uid == 0)
            continue;

        /* Same uid already represented by a lower pid (cgroup.procs is
         * pid-sorted ascending): this pid loses the merge regardless of its
         * adj, and can never be the 2nd-uid tiebreak. Skip 1 syscall. */
        if ((cand.pid != 0 && cand.uid == uid && pid > cand.pid) ||
            (second.pid != 0 && second.uid == uid && pid > second.pid))
            continue;

        if (cand.pid == 0) {
            cand.pid = pid;
            cand.uid = uid;
        } else if (cand.uid == uid) {
            /* same uid again: keep the lowest pid as the representative */
            if (pid < cand.pid)
                cand.pid = pid;
        } else if (second.pid == 0) {
            second.pid = pid;
            second.uid = uid;
        } else if (second.uid == uid) {
            if (pid < second.pid)
                second.pid = pid;
        } else {
            /* 3rd distinct real uid at adj 0: split-screen / transient */
            DBG("dbg: 3rd uid bail");
            Fg none = {0, 0};
            return none;
        }
    }

    /* Sole candidate returns unvetted; the emit path vets + names it in a
     * single cmdline read (vet_pkg below). */
    if (second.pid == 0)
        return cand;
    bool c1 = cmdline_not_app(procfd, cand.pid);
    bool c2 = cmdline_not_app(procfd, second.pid);
    if (!c1 && c2)
        return cand;
    if (c1 && !c2)
        return second;
    Fg none = {0, 0};
    return none; /* both or neither are app processes: split-screen */
}
#endif

#ifndef FGRES_TEST
static void emit_fg(Fg fg, const char *pkg, size_t plen) {
    /* logcat single line: "fg pid=<pid> uid=<uid> pkg=<pkg>".
     * plen > 0 always (all callers vet first); no empty branch. */
    char msg[320]; /* 7+10 + 5+10 + 5 + 255 pkg + NUL */
    size_t i = write_u32(msg, 0, "fg pid=", fg.pid);
    i = write_u32(msg, i, " uid=", fg.uid);
    memcpy(msg + i, " pkg=", 5);
    i += 5;
    memcpy(msg + i, pkg, plen);
    i += plen;
    log_print(4, msg, i); /* ANDROID_LOG_INFO */
}
#endif

#ifndef FGRES_TEST
int main(int argc, char **argv) {
    const char *soe = getenv("FGRES_STDOUT");
    stdout_listener = isatty(STDOUT_FILENO) ||
        (soe != NULL && soe[0] != '\0' && soe[0] != '0');
    /* stdout is best-effort: a terminal/pipe consumer that stops draining (or
     * goes away) must never stall or kill the event loop. Ignore SIGPIPE so a
     * closed pty/pipe yields EPIPE, and go non-blocking so a full one yields
     * EAGAIN; either way the write is skipped and logcat still gets the line.
     * The non-blocking flag lives on the shared open file description, so it
     * is set only when stdout is actually used — never leak EAGAIN into a
     * parent shell/pipe that outlives us.
     * ponytail: partial non-blocking writes are left as-is, listener fidelity
     * is cosmetic next to loop liveness. */
    signal(SIGPIPE, SIG_IGN);
    if (stdout_listener) {
        int sfl = fcntl(STDOUT_FILENO, F_GETFL);
        if (sfl >= 0)
            fcntl(STDOUT_FILENO, F_SETFL, sfl | O_NONBLOCK);
    }

    /* Test hook: argv[1] = watch path override, argv[2] = /proc root override
     * (emulation harness builds a fake proc tree; defaults keep prod behavior). */
    static char wbuf[PATHMAX + 1];
    static char pbuf[PATHMAX + 1];
    const char *cpath = CPATH;
    const char *proot = "/proc";
    if (argc > 1) {
        size_t l = strlen(argv[1]);
        if (l > PATHMAX)
            die("watch path too long");
        memcpy(wbuf, argv[1], l + 1);
        cpath = wbuf;
    }
    if (argc > 2) {
        size_t l = strlen(argv[2]);
        if (l > PATHMAX)
            die("proc root too long");
        memcpy(pbuf, argv[2], l + 1);
        proot = pbuf;
    }

    /* 1. inotify on the cgroup membership files: IN_MODIFY (writes) +
     *    IN_ATTRIB (some kernels rewrite via truncate, surfacing as IN_ATTRIB).
     *    Two watches: cgroup.procs writes notify on THEIR inode only, so a
     *    migration done via the sibling `tasks` file (echo PID > .../tasks,
     *    what some OEM AMS paths and every manual shell do) would otherwise be
     *    invisible. Payload is always read from cpath; either watch just means
     *    "something moved, re-read". tasks watch is best-effort (cgroup v2
     *    unified has no tasks file). */
    int fd = inotify_init1(IN_CLOEXEC);
    if (fd < 0)
        die("inotify_init1 failed");
    if (inotify_add_watch(fd, cpath, IN_MODIFY | IN_ATTRIB) < 0)
        die("inotify_add_watch failed (is /dev/cpuset mounted?)");
    {
        char tpath[PATHMAX + 1];
        size_t l = strlen(cpath);
        const char *base = "cgroup.procs";
        size_t bl = 12;
        if (l > bl && strcmp(cpath + l - bl, base) == 0 && l - bl + 6 <= sizeof(tpath)) {
            memcpy(tpath, cpath, l - bl);
            memcpy(tpath + l - bl, "tasks", 6); /* incl. NUL */
            inotify_add_watch(fd, tpath, IN_MODIFY | IN_ATTRIB); /* optional */
        }
    }

    /* 2. hold an open dirfd on the proc root: openat/fstatat start the VFS
     *    walk directly inside /proc — no "/proc/..." prefix per lookup. */
    int procfd = open(proot, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (procfd < 0)
        die("open proc root failed");

    Fg last_fg = {0, 0};
    /* Stage 1 dedupe: memcmp vs the latched payload beats hashing. Reads land
     * in tmp (stack); latched (BSS) only updates on a successful latch, so a
     * transient (unlatched) payload identical to the next one still retries. */
    static uint8_t latched[BUFSZ];
    size_t last_len = 0;
    uint8_t tmp[BUFSZ];
    char pkg[PKGMAX + 1];

    /* Initial pass: resolve once at startup so a daemon launched mid-static-state
     * (inotify is silent until the next transition) reports current fg immediately. */
    ssize_t n = (ssize_t)read_file(cpath, tmp, sizeof(tmp));
    if (n > 0 && (size_t)n != sizeof(tmp)) { /* == cap: truncated, skip */
        Fg fg = resolve(tmp, (size_t)n, procfd);
        if (fg.pid != 0) {
            size_t plen = vet_pkg(procfd, fg.pid, pkg);
            if (plen == 0) {
                /* same runner-up fallback as the event loop: a stale
                 * ':service' subprocess as initial winner must not silence
                 * startup until the next transition. */
                uint32_t alt = retry_same_uid(tmp, (size_t)n, procfd,
                                              fg.uid, fg.pid, pkg);
                if (alt != 0) {
                    plen = strlen(pkg);
                    fg.pid = alt;
                }
            }
            if (plen != 0) {
                memcpy(latched, tmp, (size_t)n);
                last_len = (size_t)n;
                last_fg = fg;
                emit_fg(fg, pkg, plen);
            }
        }
    }

    /* 3. event loop: block on read(fd) — zero CPU between events.
     * File watch => event->len == 0 always (16B/event); 64B drains up to 4
     * coalesced events per wakeup; surplus events just retrigger a read. */
    char evbuf[64];
    for (;;) {
        ssize_t rn = read(fd, evbuf, sizeof(evbuf));
        if (rn < 0) {
            if (errno == EINTR)
                continue;
            die("inotify read failed"); /* persistent fd error: never spin */
        }
        if (rn < (ssize_t)sizeof(struct inotify_event))
            continue; /* short read: re-block */

        /* 4. read the cgroup.procs payload. Fresh open every time: cgroup v1
         * caches the pidlist per open fd for 1s (cgroup_pidlist_start/stop),
         * so reusing an fd would serve stale snapshots during AMS bursts. */
        ssize_t plen = (ssize_t)read_file(cpath, tmp, sizeof(tmp));
        if (plen <= 0 || (size_t)plen == sizeof(tmp)) {
            /* transient empty mid-burst, or suspect truncation at cap — next
             * event settles it */
            DBG("dbg: empty/trunc read");
            continue;
        }

        /* 5. stage 1 dedupe: NEON memcmp vs the latched payload — exact, no
         *    hash to compute, no theoretical collisions (~120B = 8 vector cmps) */
        if ((size_t)plen == last_len && memcmp(latched, tmp, (size_t)plen) == 0) {
            DBG("dbg: dedupe1 hit");
            continue;
        }

        /* 6. resolve: parse pids, stat app_id gate, adj == 0, tiebreak */
        Fg fg = resolve(tmp, (size_t)plen, procfd);
        if (fg.pid == 0) {
            /* transient (adj not yet settled) or true split-screen. Do NOT latch
             * the payload: AMS can move the pid into top-app before raising adj
             * to 0, and the identical payload recurs once adj settles — latching
             * here would eat the real transition on the next event. */
            DBG("dbg: resolve none");
            continue;
        }

        /* 7. stage 2 dedupe: same settled fg, changed payload (pids shuffled
         *    underneath). Latch it, skip the package work entirely. */
        if (fg.pid == last_fg.pid && fg.uid == last_fg.uid) {
            DBG("dbg: dedupe2 hit");
            memcpy(latched, tmp, (size_t)plen);
            last_len = (size_t)plen;
            continue;
        }

        /* 8. emit path: package name is the winner's cmdline argv[0]. */
        size_t pl = vet_pkg(procfd, fg.pid, pkg);
        if (pl == 0) {
            /* Winner failed vetting (stale ':service' subprocess outliving a
             * restarted main process, or transient name): try the next gated
             * pid of the same uid from this same payload before giving up. */
            uint32_t alt = retry_same_uid(tmp, (size_t)plen, procfd,
                                          fg.uid, fg.pid, pkg);
            if (alt == 0)
                continue; /* no runner-up: no latch, no emit */
            pl = strlen(pkg);
            fg.pid = alt;
        }

        /* latch the payload only on a successful emit-ready resolve */
        memcpy(latched, tmp, (size_t)plen);
        last_len = (size_t)plen;
        last_fg = fg;

        /* 9. emit: log (logcat/stderr) always, stdout only with a listener */
        emit_fg(fg, pkg, pl);
    }
}
#else
/* ---- unit tests: cc -std=c11 -DFGRES_TEST fgres.c -o fgres_test && ./fgres_test ---- */
#include <assert.h>

int main(void) {
    /* test_scan_pids: kernel "<pid>\n" payloads, single pass */
    {
        const uint8_t *s1 = (const uint8_t *)"1\n22\n333\n";
        const uint8_t *s2 = (const uint8_t *)"\n\n";
        const uint8_t *s3 = (const uint8_t *)"9\n8\n";
        uint32_t pv[8];
        assert(scan_pids(s1, s1 + 10, pv, 8) == 3);
        assert(pv[0] == 1 && pv[1] == 22 && pv[2] == 333);
        assert(scan_pids(s2, s2 + 2, pv, 8) == 0);
        assert(scan_pids(s3, s3 + 4, pv, 1) == 1); /* cap */
    }

    /* test_fmt_proc_path */
    char p[64];
    size_t n = fmt_proc_path(p, 1234, "oom_score_adj");
    assert(n == 18 && memcmp(p, "1234/oom_score_adj", 18) == 0);
    n = fmt_proc_path(p, 7, "comm");
    assert(n == 6 && memcmp(p, "7/comm", 6) == 0);
    n = fmt_proc_path(p, 29514, "task");
    assert(n == 10 && memcmp(p, "29514/task", 10) == 0);

    /* test_write_u32: decimal rendering with literal prefix */
    char w[32];
    n = write_u32(w, 0, "fg pid=", 28452);
    assert(n == 12 && memcmp(w, "fg pid=28452", 12) == 0);
    n = write_u32(w, n, " uid=", 10136);
    assert(n == 22 && memcmp(w, "fg pid=28452 uid=10136", 22) == 0);

    printf("fgres_test: all assertions passed\n");
    return 0;
}
#endif
