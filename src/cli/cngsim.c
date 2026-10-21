/*
 * cngsim — Cooja-NG for people.
 *
 *   cngsim run <sim.yaml | example | firmware...> [options]
 *   cngsim new [template] [options]
 *   cngsim check <sim.yaml>
 *   cngsim examples | boards | convert <sim.csc> | help [command]
 *
 * A frontend, not a second simulator: `run` calls the same entry point as
 * `test_runner test` (run_mixed_multinode_test), with the same arguments
 * spelled for people, so a simulation cannot differ between the two
 * binaries.  What differs is what reaches the terminal.  test_runner
 * prints everything (it is what the determinism gates diff); cngsim runs
 * the simulation with its own stdout and stderr fed through a filter that
 * keeps the node consoles, the checks and a one-line verdict, and drops
 * the radio traces, ELF patch details and chip counters unless --trace or
 * -v asks for them.  With --shell or --script the output is the session
 * itself and passes through untouched.
 *
 * Bundled examples and firmware come from the data directory: $CNGSIM_DATA,
 * else <prefix>/share/cooja-ng next to an installed bin/cngsim, else the
 * checkout that build/cngsim was built in.
 *
 * docs/quick-start.md is the user guide; the plan behind it is
 * docs/design/release-0.3.1-usability-plan.md.
 */
#include "csim_version.h"
#include "sim_board.h"
#include "sim_config.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

extern char **environ;

/* The runner (test/test_mixed_multinode.c) and its two cngsim hooks. */
extern int run_mixed_multinode_test(int argc, char **argv);
extern int csim_cli_enter_resumes;
extern int csim_cli_pause_at_end;

#define EXIT_INVALID 2

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("cngsim: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int has_suffix(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

static int is_config_path(const char *p) {
    return has_suffix(p, ".yaml") || has_suffix(p, ".yml") || has_suffix(p, ".json");
}

static int is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ---- data directory ------------------------------------------------------ */

static int exe_path(char *out, size_t n, const char *argv0) {
    char buf[PATH_MAX];
#if defined(__APPLE__)
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0 && realpath(buf, out)) return 0;
#elif defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        snprintf(out, n, "%s", buf);
        return 0;
    }
#elif defined(__FreeBSD__)
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t sz = sizeof(buf);
    if (sysctl(mib, 4, buf, &sz, NULL, 0) == 0) {
        snprintf(out, n, "%s", buf);
        return 0;
    }
#endif
    if (strchr(argv0, '/') && realpath(argv0, buf)) {
        snprintf(out, n, "%s", buf);
        return 0;
    }
    (void)n;
    return -1;
}

static char g_argv0[PATH_MAX];
static char g_data[PATH_MAX];

/* The directory holding examples/ and firmware/, or NULL. */
static const char *data_dir(void) {
    if (g_data[0]) return g_data;
    const char *env = getenv("CNGSIM_DATA");
    if (env && *env) {
        snprintf(g_data, sizeof(g_data), "%s", env);
        return g_data;
    }
    char exe[PATH_MAX];
    if (exe_path(exe, sizeof(exe), g_argv0) != 0) return NULL;
    char *slash = strrchr(exe, '/');
    if (!slash) return NULL;
    *slash = '\0';                                   /* exe = its directory */
    const char *cands[] = { "/../share/cooja-ng", "/.." };
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        char d[PATH_MAX], ex[PATH_MAX + 16];
        snprintf(d, sizeof(d), "%s%s", exe, cands[i]);
        snprintf(ex, sizeof(ex), "%s/examples", d);
        if (is_dir(ex) && realpath(d, g_data)) return g_data;
    }
    return NULL;
}

static int example_path(const char *name, char *out, size_t n) {
    const char *d = data_dir();
    if (!d || strchr(name, '/')) return -1;
    snprintf(out, n, "%s/examples/%s.yaml", d, name);
    return is_file(out) ? 0 : -1;
}

/* ---- examples, boards ---------------------------------------------------- */

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int cmd_examples(void) {
    const char *d = data_dir();
    char dir[PATH_MAX + 16];
    if (!d) {
        fail("no examples found: set CNGSIM_DATA to the directory that holds "
             "examples/ and firmware/");
        return EXIT_INVALID;
    }
    snprintf(dir, sizeof(dir), "%s/examples", d);
    DIR *dp = opendir(dir);
    if (!dp) {
        fail("cannot read %s", dir);
        return EXIT_INVALID;
    }
    char *names[256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dp)) && n < 256) {
        if (e->d_name[0] != '.' && has_suffix(e->d_name, ".yaml"))
            names[n++] = strdup(e->d_name);
    }
    closedir(dp);
    qsort(names, (size_t)n, sizeof(names[0]), cmp_str);
    printf("Examples (cngsim run <name>, add --ui to watch it in the browser):\n\n");
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        names[i][strlen(names[i]) - 5] = '\0';
        static sim_normalized_config_t cfg;
        const char *title = "";
        if (sim_config_load(&cfg, path) == 0) title = cfg.title;
        printf("  %-22s %s\n", names[i], title);
        sim_config_free(&cfg);
        free(names[i]);
    }
    printf("\nThe files are in %s: copy one to start your own.\n", dir);
    return 0;
}

static int cmd_boards(void) {
    printf("%-20s %-24s %-19s %-27s %s\n", "board", "", "cpu", "radio",
           "build with (Contiki-NG)");
    for (int i = 0;; i++) {
        const sim_board_desc_t *b = sim_board_at(i);
        if (!b) break;
        if (!b->name) continue;                   /* host-process kinds */
        int dup = 0;
        for (int j = 0; j < i; j++) {
            const sim_board_desc_t *o = sim_board_at(j);
            if (o->name && strcmp(o->name, b->name) == 0) dup = 1;
        }
        if (dup) continue;
        printf("%-20s %-24s %-19s %-27s %s\n", b->name, b->title, b->cpu,
               b->radio, b->build);
    }
    printf("\nAn image named <app>.<board> runs as that board.  For any other name\n"
           "(.elf, .nrf, .simplelink, .zoul ...), add  board: <board>  to its mote\n"
           "type.  Native Cooja motes (TARGET=cooja, .cooja) run too.\n");
    return 0;
}

/* ---- output filter (run) -------------------------------------------------- */

enum { TRACE_NONE = 0, TRACE_RADIO = 1, TRACE_BOOT = 2, TRACE_STATS = 4,
       TRACE_ALL = 7 };

typedef struct {
    int fd;                 /* read end */
    int out;                /* where filtered lines go */
    int is_stdout;
} filt_arg_t;

static struct {
    int quiet, trace, ui, paused_ui, enter_ok, no_open;
    int running;            /* "--- Simulation running ---" seen */
    char title[160];
    int nodes, seed;
    double duration_s;
    char verdict[16];
    char fail_reason[400];
    double test_end_s, wall_s, sim_s, speed;
    int have_test_end, have_wall, have_sim, have_speed;
} F;

static void out_line(int fd, const char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static void out_fmt(int fd, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out_line(fd, buf);
}

static void open_browser(const char *url) {
#if defined(__APPLE__)
    const char *opener = "open";
#else
    const char *opener = "xdg-open";
#endif
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char *argv[] = { (char *)opener, (char *)url, NULL };
    pid_t pid;
    if (posix_spawnp(&pid, opener, &fa, NULL, argv, environ) == 0)
        (void)pid;            /* reaped with the others when cngsim exits */
    posix_spawn_file_actions_destroy(&fa);
}

static int skip_ws(const char *s) {
    int i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    return i;
}

/* "   12.345 [Node 2/MSP430] ..." */
static int is_console_line(const char *s) {
    int i = skip_ws(s);
    if (!isdigit((unsigned char)s[i])) return 0;
    while (isdigit((unsigned char)s[i])) i++;
    if (s[i++] != '.') return 0;
    for (int k = 0; k < 3; k++)
        if (!isdigit((unsigned char)s[i++])) return 0;
    return strncmp(s + i, " [Node ", 7) == 0;
}

static void header(int out) {
    static int done;
    if (F.quiet || done) return;
    done = 1;
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "cooja-ng %s", CSIM_VERSION);
    if (F.title[0]) n += snprintf(buf + n, sizeof(buf) - (size_t)n, " · %s", F.title);
    if (F.nodes > 0)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " · %d node%s", F.nodes,
                      F.nodes == 1 ? "" : "s");
    if (F.seed) n += snprintf(buf + n, sizeof(buf) - (size_t)n, " · seed %d", F.seed);
    if (F.duration_s > 0 && !F.ui)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " · %g s", F.duration_s);
    snprintf(buf + n, sizeof(buf) - (size_t)n, "\n");
    out_line(out, buf);
}

static void stdout_line(int out, char *line) {
    const char *t = line + skip_ws(line);
    if (strncmp(t, "WebSocket UI server listening on ", 33) == 0) {
        char url[256];
        snprintf(url, sizeof(url), "%s", t + 33);
        char *sp = strchr(url, ' ');
        if (sp) *sp = '\0';
        header(out);
        out_fmt(out, "Web UI: %s\n", url);
        if (F.paused_ui)
            out_fmt(out, "Paused: press Play (or Space) in the browser%s to start.  "
                         "Ctrl-C quits.\n", F.enter_ok ? ", or Enter here," : "");
        else
            out_fmt(out, "Ctrl-C quits.\n");
        if (strstr(t, "NOT loopback")) out_fmt(out, "%s\n", t);
        if (!F.no_open) open_browser(url);
        return;
    }
    if (!F.running) {
        if (strncmp(t, "Config: ", 8) == 0) {
            snprintf(F.title, sizeof(F.title), "%s", t + 8);
        } else if (strncmp(t, "--- Simulation running ---", 26) == 0) {
            F.running = 1;
            header(out);
        } else if (F.trace & TRACE_BOOT) {
            out_fmt(out, "%s\n", line);
        } else if (strstr(t, "rror") || strstr(t, "annot ") || strstr(t, "ailed") ||
                   strstr(t, "arning")) {
            out_fmt(out, "%s\n", t);
        }
        return;
    }
    if (is_console_line(line)) {
        if (!F.quiet) out_fmt(out, "%s\n", line);
        return;
    }
    /*   TEST step 2 PASS: "Received response" matched 3/3 (node 2, 37083 ms) */
    {
        int step;
        char ok[8], pat[256];
        int got, want, node;
        long ms;
        if (sscanf(t, "TEST step %d %7[A-Z]: \"%255[^\"]\" matched %d/%d (node %d, %ld ms)",
                   &step, ok, pat, &got, &want, &node, &ms) == 7) {
            if (!F.quiet) {
                char times[32] = "";
                if (want != 1) snprintf(times, sizeof(times), " ×%d", want);
                out_fmt(out, "  %s \"%s\"%s on node %d (%.3f s)\n",
                        strcmp(ok, "PASS") == 0 ? "✓" : "✗", pat, times, node,
                        (double)ms / 1000.0);
            }
            return;
        }
    }
    /*   Step 1 [FAIL]: wait "Received response" node=2   (the end-of-run list) */
    if (strncmp(t, "Step ", 5) == 0 && strstr(t, "]: ")) {
        if (strstr(t, "[FAIL]") || strstr(t, "[PENDING]") || strstr(t, "[TIMEOUT]"))
            out_fmt(out, "  ✗ %s\n", strstr(t, "]: ") + 3);
        return;
    }
    /*   Validator [PASS]: "Received request" matched 46/1 node=1 */
    if (strncmp(t, "Validator [", 11) == 0) {
        char ok[8], pat[256];
        int got, want, node = 0;
        int k = sscanf(t, "Validator [%7[A-Z]]: \"%255[^\"]\" matched %d/%d node=%d",
                       ok, pat, &got, &want, &node);
        if (k >= 4 && !F.quiet) {
            char where[32] = "";
            if (k == 5) snprintf(where, sizeof(where), " on node %d", node);
            out_fmt(out, "  %s \"%s\"%s (seen %d, needed %d)\n",
                    strcmp(ok, "PASS") == 0 ? "✓" : "✗", pat, where, got, want);
        }
        return;
    }
    if (strncmp(t, "TEST PASSED", 11) == 0) {
        snprintf(F.verdict, sizeof(F.verdict), "PASSED");
        long ms;
        if (sscanf(t, "TEST PASSED (%ld ms simulated)", &ms) == 1) {
            F.test_end_s = (double)ms / 1000.0;
            F.have_test_end = 1;
        }
        return;
    }
    if (strncmp(t, "TEST FAILED", 11) == 0) {
        snprintf(F.verdict, sizeof(F.verdict), "FAILED");
        const char *why = t + 11;
        while (*why == ':' || *why == ' ') why++;
        snprintf(F.fail_reason, sizeof(F.fail_reason), "%s", why);
        return;
    }
    if (strncmp(t, "Wall-clock time:", 16) == 0) {
        double ms, s;
        if (sscanf(t, "Wall-clock time: %lf ms (%lf s)", &ms, &s) == 2) {
            F.wall_s = ms / 1000.0;
            F.have_wall = 1;
        }
    } else if (strncmp(t, "Simulated time:", 15) == 0) {
        long ms;
        if (sscanf(t, "Simulated time: %ld ms", &ms) == 1) {
            F.sim_s = (double)ms / 1000.0;
            F.have_sim = 1;
        }
    } else if (strncmp(t, "Speed ratio:", 12) == 0) {
        if (sscanf(t, "Speed ratio: %lfx", &F.speed) == 1) F.have_speed = 1;
    } else if (strncmp(t, "Paused at the end", 17) == 0 ||
               strncmp(t, "--wall-timeout", 14) == 0 || strstr(t, "rror") ||
               strstr(t, "arning")) {
        out_fmt(out, "%s\n", t);
        return;
    }
    if (F.trace & TRACE_STATS) out_fmt(out, "%s\n", line);
}

static int is_radio_trace(const char *s) {
    const char *t = s + skip_ws(s);
    return strncmp(t, "[PKT]", 5) == 0 || strncmp(t, "[HEX]", 5) == 0 ||
           strncmp(t, "[RF]", 4) == 0 || strncmp(t, "[UIP]", 5) == 0;
}

/* A line on stderr that tells the user something went wrong.  Everything
 * else there is a developer trace: kept (the last ERR_KEEP lines) and shown
 * only if the run then fails without a verdict, where it may explain why. */
static int looks_like_error(const char *s) {
    static const char *const words[] = {
        "rror", "annot ", "ailed", "arning", "nknown", "nvalid", "issing",
        "no board", "not found", "No such", "must ", "needs ", "too many",
        "refus", "denied", "abort", "fault", "Fault",
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (strstr(s, words[i])) return 1;
    return 0;
}

#define ERR_KEEP 30
static char err_ring[ERR_KEEP][512];
static int err_count;

static void stderr_line(int out, char *line) {
    if (is_radio_trace(line)) {
        if (F.trace & TRACE_RADIO) out_fmt(out, "%s\n", line);
        return;
    }
    if (looks_like_error(line) || (F.trace & TRACE_STATS)) {
        out_fmt(out, "%s\n", line);
        return;
    }
    snprintf(err_ring[err_count % ERR_KEEP], sizeof(err_ring[0]), "%s", line);
    err_count++;
}

static void *filter_thread(void *p) {
    filt_arg_t *a = p;
    char buf[8192];
    char line[16384];
    size_t len = 0;
    for (;;) {
        ssize_t n = read(a->fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || len == sizeof(line) - 1) {
                line[len] = '\0';
                if (a->is_stdout) stdout_line(a->out, line);
                else stderr_line(a->out, line);
                len = 0;
                if (buf[i] == '\n') continue;
            }
            line[len++] = buf[i];
        }
    }
    if (len) {
        line[len] = '\0';
        if (a->is_stdout) stdout_line(a->out, line);
        else stderr_line(a->out, line);
    }
    close(a->fd);
    return NULL;
}

/* ---- run --------------------------------------------------------------- */

static int parse_duration_ms(const char *s, long *ms) {
    char *end;
    double v = strtod(s, &end);
    if (end == s || v < 0) return -1;
    double mult = 1.0;                         /* a bare number is ms, like -t */
    if (strcmp(end, "ms") == 0 || *end == '\0') mult = 1.0;
    else if (strcmp(end, "s") == 0) mult = 1000.0;
    else if (strcmp(end, "m") == 0 || strcmp(end, "min") == 0) mult = 60000.0;
    else if (strcmp(end, "h") == 0) mult = 3600000.0;
    else return -1;
    *ms = (long)(v * mult + 0.5);
    return 0;
}

#define MAX_ARGS 128

static int cmd_run(int argc, char **argv) {
    char *rargv[MAX_ARGS];
    int rargc = 0;
    const char *targets[64];
    int ntargets = 0;
    int ui = 0, start = 0, verbose = 0, passthrough = 0, have_speed = 0;
    const char *port = "8080";
    long duration_ms = -1;
    static char numbuf[8][32];
    int nb = 0;

    memset(&F, 0, sizeof(F));
#define PUSH(x) do { if (rargc >= MAX_ARGS - 1) { fail("too many arguments"); return EXIT_INVALID; } \
                     rargv[rargc++] = (char *)(x); } while (0)
#define NEED_VALUE() do { if (i + 1 >= argc) { fail("run: %s needs a value", a); return EXIT_INVALID; } } while (0)
    char *opts[MAX_ARGS];
    int nopts = 0;
#define OPT(x) do { if (nopts >= MAX_ARGS - 1) { fail("too many arguments"); return EXIT_INVALID; } \
                    opts[nopts++] = (char *)(x); } while (0)
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--ui") == 0) {
            ui = 1;
            if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) port = argv[++i];
        } else if (strcmp(a, "--no-open") == 0) {
            F.no_open = 1;
        } else if (strcmp(a, "--start") == 0) {
            start = 1;
        } else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
            F.quiet = 1;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(a, "--trace") == 0) {
            NEED_VALUE();
            const char *k = argv[++i];
            if (strcmp(k, "radio") == 0 || strcmp(k, "packets") == 0) F.trace |= TRACE_RADIO;
            else if (strcmp(k, "boot") == 0) F.trace |= TRACE_BOOT;
            else if (strcmp(k, "stats") == 0) F.trace |= TRACE_STATS;
            else if (strcmp(k, "all") == 0) F.trace = TRACE_ALL;
            else {
                fail("run: --trace takes radio, boot, stats or all");
                return EXIT_INVALID;
            }
        } else if (strcmp(a, "--duration") == 0 || strcmp(a, "-t") == 0) {
            NEED_VALUE();
            if (parse_duration_ms(argv[++i], &duration_ms) != 0) {
                fail("run: bad duration '%s' (try 60s, 500ms, 2m)", argv[i]);
                return EXIT_INVALID;
            }
            snprintf(numbuf[nb], sizeof(numbuf[nb]), "%ld", duration_ms);
            OPT("-t");
            OPT(numbuf[nb++]);
        } else if (strcmp(a, "--speed") == 0) {
            NEED_VALUE();
            have_speed = 1;
            OPT(a);
            OPT(argv[++i]);
        } else if (strcmp(a, "--seed") == 0 || strcmp(a, "--save-config") == 0 ||
                   strcmp(a, "--pcap") == 0 || strcmp(a, "--plugin") == 0 ||
                   strcmp(a, "--ui-record") == 0 || strcmp(a, "--ui-bind") == 0 ||
                   strcmp(a, "--wall-timeout") == 0 || strcmp(a, "-n") == 0 ||
                   strcmp(a, "--gdb") == 0 || strcmp(a, "--renode-freq") == 0) {
            NEED_VALUE();
            OPT(a);
            OPT(argv[++i]);
        } else if (strcmp(a, "--script") == 0) {
            NEED_VALUE();
            passthrough = 1;
            OPT(a);
            OPT(argv[++i]);
        } else if (strcmp(a, "--shell") == 0) {
            passthrough = 1;
            OPT(a);
        } else if (strcmp(a, "--paused") == 0 || strcmp(a, "--ui-edit") == 0 ||
                   strcmp(a, "--gdb-wait") == 0 || strcmp(a, "--realtime") == 0 ||
                   strcmp(a, "--renode") == 0) {
            OPT(a);
        } else if (a[0] == '-') {
            fail("run: unknown option '%s' (cngsim help run)", a);
            return EXIT_INVALID;
        } else {
            if (ntargets >= 64) {
                fail("run: too many files");
                return EXIT_INVALID;
            }
            targets[ntargets++] = a;
        }
    }
    if (ntargets == 0) {
        fail("run: give a setup file, an example name (cngsim examples) or "
             "firmware images");
        return EXIT_INVALID;
    }

    /* One setup file, an example by name, or firmware images. */
    static char example[PATH_MAX + 64];
    static sim_normalized_config_t cfg;
    int have_cfg = 0;
    if (ntargets == 1 && (is_config_path(targets[0]) ||
                          (!is_file(targets[0]) &&
                           example_path(targets[0], example, sizeof(example)) == 0))) {
        const char *cfg_path = is_config_path(targets[0]) ? targets[0] : example;
        if (!is_file(cfg_path)) {
            fail("run: %s: no such file", cfg_path);
            return EXIT_INVALID;
        }
        if (sim_config_load(&cfg, cfg_path) != 0) return EXIT_INVALID;
        have_cfg = 1;
        PUSH(cfg_path);
        snprintf(F.title, sizeof(F.title), "%s", cfg.title);
        F.nodes = cfg.node_count;
        F.seed = cfg.seed;
        F.duration_s = cfg.timeout_ms / 1000.0;
    } else {
        for (int i = 0; i < ntargets; i++) {
            if (!is_file(targets[i])) {
                if (is_config_path(targets[i]) || !strchr(targets[i], '.'))
                    fail("run: '%s' is neither a file nor an example (cngsim examples)",
                         targets[i]);
                else
                    fail("run: %s: no such file", targets[i]);
                return EXIT_INVALID;
            }
            PUSH(targets[i]);
        }
        F.nodes = ntargets;
        F.duration_s = 20.0;
    }
    if (duration_ms >= 0) F.duration_s = duration_ms / 1000.0;
    for (int i = 0; i < nopts; i++) {
        if (strcmp(opts[i], "--seed") == 0 && i + 1 < nopts) F.seed = atoi(opts[i + 1]);
        if (strcmp(opts[i], "-n") == 0 && i + 1 < nopts) F.nodes = atoi(opts[i + 1]);
        PUSH(opts[i]);
    }

    if (ui) {
        PUSH("--ui");
        PUSH(port);
        if (!start) PUSH("--paused");
        /* Real time unless asked otherwise: the speed a person can follow. */
        if (!have_speed && !(have_cfg && cfg.speed > 0)) {
            PUSH("--speed");
            PUSH("1");
        }
        F.ui = 1;
        F.paused_ui = !start;
        F.enter_ok = !start && !passthrough && isatty(STDIN_FILENO);
        csim_cli_enter_resumes = F.enter_ok;
        csim_cli_pause_at_end = 1;
    }
    rargv[rargc] = NULL;

    if (verbose || passthrough) {
        if (verbose && !passthrough) F.trace = TRACE_ALL;
        return run_mixed_multinode_test(rargc, rargv);
    }

    /* The simulation's stdout and stderr go through the filter threads. */
    int po[2], pe[2];
    if (pipe(po) != 0 || pipe(pe) != 0) {
        perror("cngsim: pipe");
        return run_mixed_multinode_test(rargc, rargv);
    }
    fflush(stdout);
    fflush(stderr);
    int saved_out = dup(STDOUT_FILENO), saved_err = dup(STDERR_FILENO);
    dup2(po[1], STDOUT_FILENO);
    dup2(pe[1], STDERR_FILENO);
    close(po[1]);
    close(pe[1]);
    setvbuf(stdout, NULL, _IOLBF, 0);
    filt_arg_t fo = { po[0], saved_out, 1 }, fe = { pe[0], saved_err, 0 };
    pthread_t to, te;
    pthread_create(&to, NULL, filter_thread, &fo);
    pthread_create(&te, NULL, filter_thread, &fe);

    int rc = run_mixed_multinode_test(rargc, rargv);

    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, STDOUT_FILENO);       /* closes the pipes' last write ends */
    dup2(saved_err, STDERR_FILENO);
    pthread_join(to, NULL);
    pthread_join(te, NULL);
    close(saved_out);
    close(saved_err);

    if (rc != 0 && !F.verdict[0] && err_count > 0) {
        /* Failed without a verdict: the held-back stderr may say why. */
        int from = err_count > ERR_KEEP ? err_count - ERR_KEEP : 0;
        fprintf(stderr, "(the last lines the simulator wrote to stderr:)\n");
        for (int k = from; k < err_count; k++)
            fprintf(stderr, "  %s\n", err_ring[k % ERR_KEEP]);
    }
    if (rc == EXIT_INVALID && !F.running) return rc;    /* the error said it all */
    const char *word = F.verdict[0] ? F.verdict
                     : rc == 0 ? "DONE" : rc == 1 ? "FAILED"
                     : rc == 6 ? "WALL TIMEOUT" : rc == 7 ? "CANCELLED" : "ERROR";
    char summary[512];
    int n = snprintf(summary, sizeof(summary), "%s", word);
    if (F.verdict[0] && strcmp(F.verdict, "FAILED") == 0 && F.fail_reason[0])
        n += snprintf(summary + n, sizeof(summary) - (size_t)n, ": %s", F.fail_reason);
    double sim_s = F.have_test_end ? F.test_end_s : F.sim_s;
    if ((F.have_test_end || F.have_sim) && F.have_wall) {
        n += snprintf(summary + n, sizeof(summary) - (size_t)n,
                      sim_s < 1.0 ? " · %.3f s simulated in %.2f s"
                                  : " · %.1f s simulated in %.2f s", sim_s, F.wall_s);
        if (F.wall_s > 0 && sim_s > 0)
            snprintf(summary + n, sizeof(summary) - (size_t)n, " (%.0fx real time)",
                     sim_s / F.wall_s);
    }
    printf("%s\n", summary);
    return rc;
}

/* ---- check ------------------------------------------------------------- */

static int cmd_check(int argc, char **argv) {
    if (argc != 1) {
        fail("check: give one setup file");
        return EXIT_INVALID;
    }
    char ex[PATH_MAX + 64];
    const char *path = argv[0];
    if (!is_file(path) && example_path(path, ex, sizeof(ex)) == 0) path = ex;
    static sim_normalized_config_t cfg;
    if (sim_config_load(&cfg, path) != 0) {
        printf("%s: INVALID\n", argv[0]);
        return EXIT_INVALID;
    }
    int bad = 0;
    for (int t = 0; t < cfg.mote_type_count; t++) {
        const sim_mote_type_t *mt = &cfg.mote_types[t];
        const char *fw = mt->firmware[0] ? mt->firmware : cfg.mote_type_firmware[t];
        if (!fw[0]) continue;
        char err[512];
        const sim_board_desc_t *b = sim_board_resolve(fw, mt->board, err, sizeof(err));
        if (!b) {
            fprintf(stderr, "%s: %s\n", argv[0], err);
            bad++;
        } else if (b->kind != SIM_BOARD_KIND_RENODE && access(fw, R_OK) != 0) {
            fprintf(stderr, "%s: mote type %s: cannot read %s\n", argv[0],
                    mt->name[0] ? mt->name : "?", fw);
            bad++;
        }
        if (mt->secure_firmware[0] && access(mt->secure_firmware, R_OK) != 0) {
            fprintf(stderr, "%s: cannot read %s\n", argv[0], mt->secure_firmware);
            bad++;
        }
    }
    if (cfg.mote_type_count == 0) {          /* v1: firmware on each node */
        for (int i = 0; i < cfg.node_count; i++) {
            char err[512];
            if (!sim_board_resolve(cfg.nodes[i].firmware, NULL, err, sizeof(err))) {
                fprintf(stderr, "%s: %s\n", argv[0], err);
                bad++;
            } else if (access(cfg.nodes[i].firmware, R_OK) != 0) {
                fprintf(stderr, "%s: node %d: cannot read %s\n", argv[0],
                        cfg.nodes[i].id, cfg.nodes[i].firmware);
                bad++;
            }
        }
    }
    if (cfg.has_js_script && cfg.js_script_path[0] && access(cfg.js_script_path, R_OK) != 0) {
        fprintf(stderr, "%s: cannot read js_script %s\n", argv[0], cfg.js_script_path);
        bad++;
    }
    if (bad) {
        printf("%s: INVALID\n", argv[0]);
        sim_config_free(&cfg);
        return EXIT_INVALID;
    }
    printf("%s: OK · %d node%s, %d mote type%s, %g s", argv[0], cfg.node_count,
           cfg.node_count == 1 ? "" : "s", cfg.mote_type_count,
           cfg.mote_type_count == 1 ? "" : "s", cfg.timeout_ms / 1000.0);
    if (cfg.has_test)
        printf(", %d check%s", cfg.test.step_count + cfg.test.validator_count,
               cfg.test.step_count + cfg.test.validator_count == 1 ? "" : "s");
    else if (cfg.has_js_script)
        printf(", a JavaScript test");
    printf("\n");
    for (int t = 0; t < cfg.mote_type_count; t++) {
        const sim_mote_type_t *mt = &cfg.mote_types[t];
        char err[512];
        const sim_board_desc_t *b = sim_board_resolve(mt->firmware, mt->board, err, sizeof(err));
        int users = 0;
        for (int i = 0; i < cfg.node_count; i++)
            if (strcmp(cfg.nodes[i].type_name, mt->name) == 0) users++;
        printf("  %-12s %-20s %2d node%s  %s\n", mt->name,
               b ? (b->name ? b->name : b->label) : "?", users, users == 1 ? " " : "s",
               mt->firmware);
    }
    sim_config_free(&cfg);
    return 0;
}

/* ---- new --------------------------------------------------------------- */

typedef struct {
    const char *name, *blurb;
    const char *roles[2], *images[2];
    const char *expect;                   /* console line the check waits for */
} template_t;

static const template_t templates[] = {
    { "rpl-udp", "an RPL network: node 1 is the UDP server and the RPL root, the "
      "others are clients that send it requests",
      { "server", "client" }, { "udp-server", "udp-client" }, "Received response" },
    { "nullnet", "broadcast without IP: every node broadcasts and prints what "
      "it receives", { "node", NULL }, { "nullnet-broadcast", NULL }, "Received" },
    { "hello", "hello world on every node", { "node", NULL }, { "hello-world", NULL },
      "Hello, world" },
};

static const template_t *find_template(const char *name) {
    for (size_t i = 0; i < sizeof(templates) / sizeof(templates[0]); i++)
        if (strcmp(templates[i].name, name) == 0) return &templates[i];
    return NULL;
}

/* `path` as the YAML should name it from `dir`: relative when that stays
 * short, else absolute ("~/..." under the home directory). */
static void path_from(const char *dir, const char *path, char *out, size_t n) {
    char a[PATH_MAX], b[PATH_MAX];
    if (!realpath(dir, a) || !realpath(path, b)) {
        snprintf(out, n, "%s", path);
        return;
    }
    size_t la = strlen(a), i = 0, common;
    while (a[i] && a[i] == b[i]) i++;
    if (a[i] == '\0' && b[i] == '/') {
        common = i;                       /* the file is under dir */
    } else {
        while (i > 0 && a[i - 1] != '/') i--;
        common = i ? i - 1 : 0;           /* the last '/' both share */
    }
    int ups = 0;
    if (common < la) {
        ups = 1;
        for (size_t k = common + 1; k < la; k++)
            if (a[k] == '/') ups++;
    }
    if (common > 0 && ups <= 2) {
        out[0] = '\0';
        for (int k = 0; k < ups; k++) strncat(out, "../", n - strlen(out) - 1);
        strncat(out, b + common + 1, n - strlen(out) - 1);
        return;
    }
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (hl && strncmp(b, home, hl) == 0 && b[hl] == '/')
        snprintf(out, n, "~%s", b + hl);
    else
        snprintf(out, n, "%s", b);
}

static void prompt(const char *q, const char *dflt, char *out, size_t n) {
    printf("%s [%s]: ", q, dflt);
    fflush(stdout);
    if (!fgets(out, (int)n, stdin)) out[0] = '\0';
    out[strcspn(out, "\r\n")] = '\0';
    char *s = out;
    while (*s == ' ') s++;
    memmove(out, s, strlen(s) + 1);
    if (!out[0]) snprintf(out, n, "%s", dflt);
}

static int cmd_new(int argc, char **argv) {
    char tname[64] = "rpl-udp", board[64] = "sky", layout[16] = "grid";
    char outp[PATH_MAX] = "sim.yaml", nbuf[16] = "4";
    int n = 4, seed = 42, force = 0, asked = 0;
    double spacing = 30.0;
    long duration_ms = 120000;
    int i = 0;
    if (argc > 0 && argv[0][0] != '-') snprintf(tname, sizeof(tname), "%s", argv[i++]);
    for (; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (strcmp(a, "--force") == 0) { force = 1; continue; }
        if (!more) {
            fail("new: %s needs a value (cngsim help new)", a);
            return EXIT_INVALID;
        }
        const char *v = argv[++i];
        if (strcmp(a, "--board") == 0) snprintf(board, sizeof(board), "%s", v);
        else if (strcmp(a, "-n") == 0) n = atoi(v);
        else if (strcmp(a, "--layout") == 0) snprintf(layout, sizeof(layout), "%s", v);
        else if (strcmp(a, "--spacing") == 0) spacing = atof(v);
        else if (strcmp(a, "-o") == 0) snprintf(outp, sizeof(outp), "%s", v);
        else if (strcmp(a, "--seed") == 0) seed = atoi(v);
        else if (strcmp(a, "--duration") == 0) {
            if (parse_duration_ms(v, &duration_ms) != 0) {
                fail("new: bad duration '%s' (try 120s, 5m)", v);
                return EXIT_INVALID;
            }
        } else {
            fail("new: unknown option '%s' (cngsim help new)", a);
            return EXIT_INVALID;
        }
    }
    if (argc == 0 && isatty(STDIN_FILENO)) {
        asked = 1;
        printf("A new simulation.  Press Enter to take the value in brackets.\n");
        prompt("Template (rpl-udp, nullnet, hello)", tname, tname, sizeof(tname));
        prompt("Board (cngsim boards lists them)", board, board, sizeof(board));
        snprintf(nbuf, sizeof(nbuf), "%d", n);
        prompt("Number of nodes", nbuf, nbuf, sizeof(nbuf));
        n = atoi(nbuf);
        prompt("Layout (grid, line, circle, random)", layout, layout, sizeof(layout));
        prompt("Write it to", outp, outp, sizeof(outp));
    }
    const template_t *tp = find_template(tname);
    if (!tp) {
        fail("new: unknown template '%s' (rpl-udp, nullnet, hello)", tname);
        return EXIT_INVALID;
    }
    if (n < 1 || n > MAX_SIM_NODES || (tp->roles[1] && n < 2)) {
        fail("new: %d nodes: give %s1 to %d", n, tp->roles[1] ? "2 (a server and a client) " : "", MAX_SIM_NODES);
        return EXIT_INVALID;
    }
    if (strcmp(layout, "grid") && strcmp(layout, "line") && strcmp(layout, "circle") &&
        strcmp(layout, "random")) {
        fail("new: unknown layout '%s' (grid, line, circle, random)", layout);
        return EXIT_INVALID;
    }
    if (spacing <= 0) {
        fail("new: --spacing must be > 0");
        return EXIT_INVALID;
    }
    const sim_board_desc_t *bd = sim_board_find(board);
    if (!bd) {
        fail("new: unknown board '%s' (cngsim boards lists them)", board);
        return EXIT_INVALID;
    }
    const char *data = data_dir();
    if (!data) {
        fail("new: no bundled firmware found: set CNGSIM_DATA to the directory "
             "that holds examples/ and firmware/");
        return EXIT_INVALID;
    }
    if (!force && access(outp, F_OK) == 0) {
        fail("new: %s exists (--force overwrites it)", outp);
        return EXIT_INVALID;
    }
    char outdir[PATH_MAX];
    snprintf(outdir, sizeof(outdir), "%s", outp);
    char *sl = strrchr(outdir, '/');
    if (sl) { if (sl == outdir) sl[1] = '\0'; else *sl = '\0'; }
    else snprintf(outdir, sizeof(outdir), ".");

    char fw[2][PATH_MAX];
    for (int r = 0; r < 2 && tp->roles[r]; r++) {
        char img[PATH_MAX + 256];
        snprintf(img, sizeof(img), "%s/firmware/%s/%s.%s", data, board, tp->images[r], board);
        if (!is_file(img)) {
            char have[512] = "";
            for (int k = 0;; k++) {
                const sim_board_desc_t *b = sim_board_at(k);
                if (!b) break;
                if (!b->name || strstr(have, b->name)) continue;
                char p[PATH_MAX + 256];
                snprintf(p, sizeof(p), "%s/firmware/%s/%s.%s", data, b->name, tp->images[r], b->name);
                if (is_file(p)) {
                    strncat(have, have[0] ? ", " : "", sizeof(have) - strlen(have) - 1);
                    strncat(have, b->name, sizeof(have) - strlen(have) - 1);
                }
            }
            fail("new: no bundled %s image for %s; the %s template comes with: %s",
                 tp->images[r], board, tp->name, have);
            return EXIT_INVALID;
        }
        path_from(outdir, img, fw[r], sizeof(fw[r]));
    }

    int clients = tp->roles[1] ? n - 1 : n;
    int columns = (int)ceil(sqrt((double)(clients > 0 ? clients : 1)));
    double range = spacing * 1.5;
    FILE *f = fopen(outp, "w");
    if (!f) {
        fail("new: cannot write %s: %s", outp, strerror(errno));
        return EXIT_INVALID;
    }
    fprintf(f, "# %c%s.\n", toupper((unsigned char)tp->blurb[0]), tp->blurb + 1);
    fprintf(f, "# Written by: cngsim new %s --board %s -n %d --layout %s\n", tp->name, board, n, layout);
    fprintf(f, "# Run it:   cngsim run %s\n# Watch it: cngsim run %s --ui\n\n", outp, outp);
    char kv[128];
#define KV(comment, ...) do { snprintf(kv, sizeof(kv), __VA_ARGS__); \
        fprintf(f, "%-26s# %s\n", kv, comment); } while (0)
    fprintf(f, "version: 2\n");
    fprintf(f, "title: \"%s on %s, %d node%s (%s)\"\n", tp->name, board, n, n == 1 ? "" : "s", layout);
    KV("how long to simulate (simulated time, ms)", "timeout_ms: %ld", duration_ms);
    KV("the same seed gives the same run; change it for another", "seed: %d", seed);
    KV("nodes boot at random times within the first second", "startup_delay_ms: 1000");
    fprintf(f, "\nmedium:\n");
    KV("unit-disk graph, as in Cooja: within range means received", "  type: udgm");
    KV("radio range, metres", "  tx_range: %g", range);
    KV("a sender this close disturbs reception", "  interference_range: %g", range * 2);
#undef KV
    fprintf(f, "\n");
    fprintf(f, "# The firmware the nodes run, one entry per image.  Paths are relative to\n");
    fprintf(f, "# this file.  An image not named <app>.<board> (an .elf, .nrf, .simplelink)\n");
    fprintf(f, "# also needs  board: <name>  (cngsim boards lists them).\n");
    fprintf(f, "mote_types:\n");
    for (int r = 0; r < 2 && tp->roles[r]; r++)
        fprintf(f, "  - { name: %s, firmware: %s }\n", tp->roles[r], fw[r]);
    fprintf(f, "\n# Positions in metres.  One entry can place many nodes: count + layout\n");
    fprintf(f, "# (grid: columns, spacing; line: spacing; circle: radius; random: width,\n");
    fprintf(f, "# height), starting at x, y.  Or list nodes one by one with an id, x and y.\n");
    fprintf(f, "nodes:\n");
    const char *role = tp->roles[1] ? tp->roles[1] : tp->roles[0];
    double x0 = 0;
    int first_id = 1;
    if (tp->roles[1]) {
        fprintf(f, "  - { type: %s, id: 1, x: 0, y: 0 }\n", tp->roles[0]);
        x0 = spacing;
        first_id = 2;
    }
    if (strcmp(layout, "grid") == 0)
        fprintf(f, "  - { type: %s, id: %d, count: %d, layout: grid, columns: %d, spacing: %g, x: %g, y: 0 }\n",
                role, first_id, clients, columns, spacing, x0);
    else if (strcmp(layout, "line") == 0)
        fprintf(f, "  - { type: %s, id: %d, count: %d, layout: line, spacing: %g, x: %g, y: 0 }\n",
                role, first_id, clients, spacing, x0);
    else if (strcmp(layout, "circle") == 0) {
        double r = spacing * clients / (2.0 * M_PI);
        if (r < spacing) r = spacing;
        fprintf(f, "  - { type: %s, id: %d, count: %d, layout: circle, radius: %g, x: 0, y: 0 }\n",
                role, first_id, clients, round(r * 10) / 10);
    } else {
        double side = round(spacing * sqrt((double)clients) * 10) / 10;
        fprintf(f, "  - { type: %s, id: %d, count: %d, layout: random, width: %g, height: %g, x: 0, y: 0 }\n",
                role, first_id, clients, side, side);
    }
    fclose(f);

    /* The check waits for the node farthest from node 1: placed by the
     * loader, so load what was written. */
    static sim_normalized_config_t cfg;
    if (sim_config_load(&cfg, outp) != 0) {
        fail("new: wrote %s, but it does not load (a bug: please report it)", outp);
        return EXIT_INVALID;
    }
    int far = cfg.nodes[cfg.node_count - 1].id;
    double best = -1;
    for (int k = tp->roles[1] ? 1 : 0; k < cfg.node_count; k++) {
        double dx = cfg.nodes[k].x - cfg.nodes[0].x, dy = cfg.nodes[k].y - cfg.nodes[0].y;
        double d = dx * dx + dy * dy;
        if (d > best) { best = d; far = cfg.nodes[k].id; }
    }
    sim_config_free(&cfg);
    f = fopen(outp, "a");
    if (!f) {
        fail("new: cannot write %s: %s", outp, strerror(errno));
        return EXIT_INVALID;
    }
    fprintf(f, "\n# Checks.  The run passes when every step is seen, in order, and exits\n");
    fprintf(f, "# with 0; otherwise with 1.  test-format.md has more (validators, fail_on,\n");
    fprintf(f, "# Cooja JavaScript test scripts).\n");
    fprintf(f, "test:\n  steps:\n");
    fprintf(f, "    - { wait: \"%s\", node: %d }%s\n", tp->expect, far,
            tp->roles[1] ? "   # the farthest client got an answer" : "");
    fclose(f);

    printf("Wrote %s: %d %s node%s, %s layout, %g m radio range.\n", outp, n, board,
           n == 1 ? "" : "s", layout, range);
    printf("  Run it:   cngsim run %s\n  Watch it: cngsim run %s --ui\n", outp, outp);
    (void)asked;
    return 0;
}

/* ---- convert ----------------------------------------------------------- */

static int cmd_convert(int argc, char **argv) {
    const char *in = NULL, *out = NULL;
    char *extra[32];
    int nextra = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
        else if ((strcmp(argv[i], "--contiki") == 0 || strcmp(argv[i], "--firmware-dir") == 0) &&
                 i + 1 < argc && nextra < 30) {
            extra[nextra++] = argv[i];
            extra[nextra++] = argv[++i];
        } else if (argv[i][0] != '-' && !in) in = argv[i];
        else {
            fail("convert: unknown option '%s' (cngsim help convert)", argv[i]);
            return EXIT_INVALID;
        }
    }
    if (!in || !has_suffix(in, ".csc")) {
        fail("convert: give a Cooja .csc file");
        return EXIT_INVALID;
    }
    char outbuf[PATH_MAX];
    if (!out) {
        snprintf(outbuf, sizeof(outbuf), "%.*s.yaml", (int)(strlen(in) - 4), in);
        out = outbuf;
    }
    const char *data = data_dir();
    char script[PATH_MAX + 64];
    snprintf(script, sizeof(script), "%s/tools/csc2json.py", data ? data : ".");
    if (!is_file(script)) {
        fail("convert: %s not found (set CNGSIM_DATA)", script);
        return EXIT_INVALID;
    }
    char tmp[PATH_MAX];
    const char *td = getenv("TMPDIR");
    snprintf(tmp, sizeof(tmp), "%s/cngsim-convert-%d.json", td && *td ? td : "/tmp", (int)getpid());
    char *pargv[48];
    int pn = 0;
    pargv[pn++] = "python3";
    pargv[pn++] = script;
    pargv[pn++] = (char *)in;
    pargv[pn++] = "--js-native";
    pargv[pn++] = "-o";
    pargv[pn++] = tmp;
    for (int i = 0; i < nextra; i++) pargv[pn++] = extra[i];
    pargv[pn] = NULL;
    pid_t pid;
    if (posix_spawnp(&pid, "python3", NULL, NULL, pargv, environ) != 0) {
        fail("convert: needs python3 on PATH");
        return EXIT_INVALID;
    }
    int st;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        unlink(tmp);
        fail("convert: %s could not be converted (see above)", in);
        return EXIT_INVALID;
    }
    static sim_normalized_config_t cfg;
    int rc = sim_config_load(&cfg, tmp);
    unlink(tmp);
    if (rc != 0) return EXIT_INVALID;
    FILE *f = fopen(out, "w");
    if (!f) {
        fail("convert: cannot write %s: %s", out, strerror(errno));
        return EXIT_INVALID;
    }
    char hdr[PATH_MAX + 64];
    snprintf(hdr, sizeof(hdr), "converted from %s by cngsim %s", in, CSIM_VERSION);
    rc = sim_config_write_yaml(&cfg, f, hdr);
    fclose(f);
    sim_config_free(&cfg);
    if (rc != 0) return EXIT_INVALID;
    printf("Wrote %s.  Run it: cngsim run %s\n", out, out);
    return 0;
}

/* ---- help -------------------------------------------------------------- */

static void help(FILE *f) {
    fprintf(f,
        "cngsim %s - run Cooja-NG network simulations\n"
        "\n"
        "  cngsim run <sim.yaml | example | firmware...> [options]   run a simulation\n"
        "  cngsim new [template] [options]                           write a new setup file\n"
        "  cngsim check <sim.yaml>                                   validate a setup, without running it\n"
        "  cngsim examples                                           list the bundled examples\n"
        "  cngsim boards                                             list the boards and how to build for them\n"
        "  cngsim convert <sim.csc> [-o sim.yaml]                    a Cooja .csc as YAML (needs python3)\n"
        "  cngsim help [command] | --version\n"
        "\n"
        "Start with:  cngsim examples   then   cngsim run <example> --ui\n"
        "Exit codes:  0 pass, 1 a check failed, 2 bad input, 6 wall-clock timeout, 7 cancelled\n"
        "Guide:       docs/quick-start.md\n", CSIM_VERSION);
}

static void help_run(void) {
    printf(
        "cngsim run <sim.yaml | example | firmware...> [options]\n"
        "\n"
        "Runs a simulation headless, as fast as it goes, until its duration or its\n"
        "checks decide it, and prints the nodes' consoles, the checks and a verdict.\n"
        "A firmware image (or several) without a setup file runs one node each.\n"
        "\n"
        "  --ui [PORT]           watch it in the browser (http://127.0.0.1:8080/): it\n"
        "                        opens paused, at real time; --start runs at once,\n"
        "                        --no-open does not open the browser\n"
        "  --ui-edit             allow adding/removing nodes from a UI on --ui-bind\n"
        "  --duration 60s        simulated time (also 500ms, 2m; a bare number is ms)\n"
        "  --seed N              another run, as reproducible as the first\n"
        "  --speed N|max|realtime  pacing (headless runs are unpaced)\n"
        "  --shell               interactive command line (docs/shell.md)\n"
        "  --script FILE         a command script with a pass/fail verdict\n"
        "  --save-config F.yaml  write the setup as it is at the end (moved/added nodes)\n"
        "  --pcap FILE           capture the radio traffic for Wireshark\n"
        "  --ui-record FILE      record the run for replay in the browser\n"
        "  --plugin LIB.so       load a plugin (service or radio medium)\n"
        "  --wall-timeout DUR    give up after this much real time (exit 6)\n"
        "  -n N                  with firmware files: the number of nodes\n"
        "  -q / -v               the verdict only / everything (test_runner's output)\n"
        "  --trace radio|boot|stats|all   add the developer output, by kind\n");
}

static void help_new(void) {
    printf(
        "cngsim new [rpl-udp|nullnet|hello] [options]\n"
        "\n"
        "Writes a commented setup file from a template and the bundled firmware.\n"
        "With no arguments on a terminal, it asks.\n"
        "\n"
        "  --board NAME        the board (default sky; cngsim boards lists them)\n"
        "  -n N                the number of nodes (default 4)\n"
        "  --layout L          grid, line, circle or random (default grid)\n"
        "  --spacing M         metres between nodes (default 30; the range is 1.5x)\n"
        "  --duration D        simulated time (default 120s)\n"
        "  --seed N            (default 42)\n"
        "  -o FILE             (default sim.yaml; --force overwrites)\n");
}

static int cmd_help(int argc, char **argv) {
    if (argc > 0 && strcmp(argv[0], "run") == 0) help_run();
    else if (argc > 0 && strcmp(argv[0], "new") == 0) help_new();
    else if (argc > 0 && strcmp(argv[0], "check") == 0)
        printf("cngsim check <sim.yaml>\n\nLoads the setup the way run would: every key known, every value\n"
               "of the right type, every firmware image found and its board known.\n"
               "Prints a summary and exits 0, or the problems and exits 2.\n");
    else if (argc > 0 && strcmp(argv[0], "convert") == 0)
        printf("cngsim convert <sim.csc> [-o sim.yaml] [--contiki DIR] [--firmware-dir DIR]\n\n"
               "Converts a Cooja simulation (Sky, Z1 and Cooja motes, UDGM, the test\n"
               "script) with tools/csc2json.py.  Needs python3.  The firmware must\n"
               "already be built; see docs/contiki-ng-testing.md.\n");
    else help(stdout);
    return 0;
}

int main(int argc, char **argv) {
    snprintf(g_argv0, sizeof(g_argv0), "%s", argv[0]);
    if (argc < 2) {
        help(stderr);
        return EXIT_INVALID;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
        printf("cngsim %s (Cooja-NG)\n", CSIM_VERSION);
        return 0;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0)
        return cmd_help(argc - 2, argv + 2);
    if (argc > 2 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0))
        return cmd_help(1, argv + 1);
    if (strcmp(cmd, "run") == 0) return cmd_run(argc - 2, argv + 2);
    if (strcmp(cmd, "new") == 0) return cmd_new(argc - 2, argv + 2);
    if (strcmp(cmd, "check") == 0) return cmd_check(argc - 2, argv + 2);
    if (strcmp(cmd, "examples") == 0) return cmd_examples();
    if (strcmp(cmd, "boards") == 0) return cmd_boards();
    if (strcmp(cmd, "convert") == 0) return cmd_convert(argc - 2, argv + 2);
    fail("unknown command '%s' (cngsim help)", cmd);
    return EXIT_INVALID;
}
