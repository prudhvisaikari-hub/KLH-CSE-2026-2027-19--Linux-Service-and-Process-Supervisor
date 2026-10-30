/*
 * supervisor.c - A lightweight Linux Service & Process Supervisor
 *
 * Features:
 *   - Reads service definitions from a config file
 *   - Starts services with fork()/exec()
 *   - Monitors children with SIGCHLD + waitpid()
 *   - Auto-restarts crashed services (up to a configurable limit)
 *   - Accepts runtime control commands (START/STOP/RESTART/PAUSE/RESUME/
 *     STATUS/SHUTDOWN) through a named pipe (FIFO), sent by the svctl tool
 *   - Uses signals (SIGSTOP/SIGCONT/SIGTERM/SIGKILL) to control services
 *   - Logs all activity with timestamps to a log file
 *   - Supports clean shutdown of every managed service
 *
 * Build:  make
 * Run:    ./supervisord services.conf
 *
 * Author: (student project)
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

/* ---------------------------------------------------------------------- */
/* Configuration limits                                                    */
/* ---------------------------------------------------------------------- */
#define MAX_SERVICES     64
#define MAX_ARGS         32
#define LINE_LEN         512
#define NAME_LEN         64
#define PATH_LEN         256

#define CMD_FIFO_PATH    "/tmp/supervisor_cmd.fifo"
#define REPLY_FIFO_PATH  "/tmp/supervisor_reply.fifo"
#define LOG_FILE_PATH    "supervisor.log"
#define STATUS_FILE_PATH "supervisor_status.txt"

/* ---------------------------------------------------------------------- */
/* Data structures                                                         */
/* ---------------------------------------------------------------------- */
typedef enum {
    ST_STOPPED = 0,
    ST_RUNNING,
    ST_PAUSED,
    ST_FAILED,
    ST_TERMINATED
} ServiceState;

static const char *state_name(ServiceState s) {
    switch (s) {
        case ST_STOPPED:    return "STOPPED";
        case ST_RUNNING:    return "RUNNING";
        case ST_PAUSED:     return "PAUSED";
        case ST_FAILED:     return "FAILED";
        case ST_TERMINATED: return "TERMINATED";
        default:            return "UNKNOWN";
    }
}

typedef struct {
    char         name[NAME_LEN];
    char         path[PATH_LEN];
    char         args_raw[LINE_LEN];   /* raw argument string from config */
    char        *argv[MAX_ARGS];       /* argv[0]=path, ..., NULL terminated */
    int          autorestart;          /* 1 = restart automatically on crash */
    int          max_restarts;         /* max auto-restarts before giving up */
    int          restart_count;        /* how many times we've restarted it */
    pid_t        pid;                  /* current pid, 0 if not running */
    ServiceState state;
    time_t       last_start;
    int          in_use;               /* slot occupied */
} Service;

static Service   services[MAX_SERVICES];
static int       service_count = 0;

/* volatile flags touched by signal handlers */
static volatile sig_atomic_t g_sigchld_flag   = 0;
static volatile sig_atomic_t g_shutdown_flag  = 0;

static int cmd_fifo_fd = -1;

/* ---------------------------------------------------------------------- */
/* Logging                                                                  */
/* ---------------------------------------------------------------------- */
static void log_msg(const char *fmt, ...) {
    FILE *fp = fopen(LOG_FILE_PATH, "a");
    time_t now = time(NULL);
    char tbuf[32];
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tmv);

    va_list ap;

    /* print to stdout (foreground console) */
    printf("[%s] ", tbuf);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);

    /* append to log file */
    if (fp) {
        fprintf(fp, "[%s] ", tbuf);
        va_start(ap, fmt);
        vfprintf(fp, fmt, ap);
        va_end(ap);
        fprintf(fp, "\n");
        fclose(fp);
    }
}

/* ---------------------------------------------------------------------- */
/* Config file parsing                                                     */
/*                                                                          */
/* Format (whitespace separated, one service per line, # = comment):       */
/*   name  path  "args string"  autorestart(0/1)  max_restarts             */
/*                                                                          */
/* Example:                                                                 */
/*   webserver /usr/bin/python3 "-m http.server 8080" 1 5                  */
/* ---------------------------------------------------------------------- */

/* Quote-aware tokenizer: splits on whitespace but treats anything between
 * matching single or double quotes as one token (quotes are stripped).
 * This lets a config line pass something like:
 *     -c 'while true; do echo hi; sleep 1; done'
 * to execv() as exactly two argv entries: "-c" and the whole loop body,
 * instead of shattering it on every internal space. */
static void split_args(Service *s) {
    /* argv[0] is always the executable path */
    int argc = 0;
    s->argv[argc++] = s->path;

    const char *p = s->args_raw;
    char token[LINE_LEN];

    while (*p && argc < MAX_ARGS - 1) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;

        size_t tlen = 0;
        token[0] = '\0';

        while (*p && !isspace((unsigned char)*p)) {
            if (*p == '\'' || *p == '"') {
                char quote = *p;
                p++; /* skip opening quote */
                while (*p && *p != quote) {
                    if (tlen < sizeof(token) - 1) token[tlen++] = *p;
                    p++;
                }
                if (*p == quote) p++; /* skip closing quote */
            } else {
                if (tlen < sizeof(token) - 1) token[tlen++] = *p;
                p++;
            }
        }
        token[tlen] = '\0';
        s->argv[argc++] = strdup(token);
    }
    s->argv[argc] = NULL;
}

static int load_config(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "ERROR: cannot open config file '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    char line[LINE_LEN];
    int lineno = 0;

    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        /* strip newline */
        line[strcspn(line, "\r\n")] = '\0';

        /* skip blanks / comments */
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;

        if (service_count >= MAX_SERVICES) {
            fprintf(stderr, "WARNING: max services reached, ignoring line %d\n", lineno);
            continue;
        }

        Service *s = &services[service_count];
        memset(s, 0, sizeof(Service));

        /* Parse: name path "args" autorestart max_restarts
         * The args field may be quoted to allow spaces. */
        char name[NAME_LEN], path_buf[PATH_LEN];
        char args_buf[LINE_LEN];
        int autorestart = 1, max_restarts = 5;

        /* Manual parse to support quoted args field */
        char *cursor = p;
        char *field;

        /* name */
        field = strsep(&cursor, " \t");
        if (!field) continue;
        strncpy(name, field, sizeof(name) - 1);
        name[sizeof(name)-1] = '\0';

        /* skip extra spaces */
        while (cursor && *cursor && isspace((unsigned char)*cursor)) cursor++;

        /* path */
        field = strsep(&cursor, " \t");
        if (!field) { fprintf(stderr, "WARNING: line %d missing path\n", lineno); continue; }
        strncpy(path_buf, field, sizeof(path_buf) - 1);
        path_buf[sizeof(path_buf)-1] = '\0';

        while (cursor && *cursor && isspace((unsigned char)*cursor)) cursor++;

        args_buf[0] = '\0';
        if (cursor && *cursor == '"') {
            /* quoted args string - read until closing quote */
            cursor++; /* skip opening quote */
            char *end = strchr(cursor, '"');
            if (end) {
                size_t len = (size_t)(end - cursor);
                if (len >= sizeof(args_buf)) len = sizeof(args_buf) - 1;
                strncpy(args_buf, cursor, len);
                args_buf[len] = '\0';
                cursor = end + 1;
                while (*cursor && isspace((unsigned char)*cursor)) cursor++;
            }
        } else if (cursor) {
            /* unquoted single token args (rare) */
            field = strsep(&cursor, " \t");
            if (field) strncpy(args_buf, field, sizeof(args_buf) - 1);
        }

        /* remaining fields: autorestart max_restarts */
        if (cursor) {
            while (*cursor && isspace((unsigned char)*cursor)) cursor++;
            field = strsep(&cursor, " \t");
            if (field && *field) autorestart = atoi(field);
            while (cursor && *cursor && isspace((unsigned char)*cursor)) cursor++;
            field = cursor ? strsep(&cursor, " \t") : NULL;
            if (field && *field) max_restarts = atoi(field);
        }

        strncpy(s->name, name, sizeof(s->name) - 1);
        s->name[sizeof(s->name) - 1] = '\0';
        strncpy(s->path, path_buf, sizeof(s->path) - 1);
        s->path[sizeof(s->path) - 1] = '\0';
        strncpy(s->args_raw, args_buf, sizeof(s->args_raw) - 1);
        s->args_raw[sizeof(s->args_raw) - 1] = '\0';
        s->autorestart  = autorestart;
        s->max_restarts = max_restarts;
        s->restart_count = 0;
        s->pid = 0;
        s->state = ST_STOPPED;
        s->in_use = 1;

        split_args(s);

        service_count++;
    }

    fclose(fp);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Service lookup                                                          */
/* ---------------------------------------------------------------------- */
static Service *find_by_name(const char *name) {
    for (int i = 0; i < service_count; i++) {
        if (services[i].in_use && strcmp(services[i].name, name) == 0)
            return &services[i];
    }
    return NULL;
}

static Service *find_by_pid(pid_t pid) {
    for (int i = 0; i < service_count; i++) {
        if (services[i].in_use && services[i].pid == pid)
            return &services[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Process control: start / stop / pause / resume                          */
/* ---------------------------------------------------------------------- */
static int start_service(Service *s) {
    if (s->state == ST_RUNNING || s->state == ST_PAUSED) {
        log_msg("START %s: already running (pid %d)", s->name, s->pid);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_msg("ERROR: fork() failed for service '%s': %s", s->name, strerror(errno));
        s->state = ST_FAILED;
        return -1;
    }

    if (pid == 0) {
        /* CHILD: replace image with the target executable */
        execv(s->path, s->argv);
        /* execv only returns on error */
        fprintf(stderr, "exec failed for %s: %s\n", s->path, strerror(errno));
        _exit(127);
    }

    /* PARENT */
    s->pid = pid;
    s->state = ST_RUNNING;
    s->last_start = time(NULL);
    log_msg("STARTED service '%s' (pid %d, path=%s)", s->name, s->pid, s->path);
    return 0;
}

static int stop_service(Service *s) {
    if (s->state != ST_RUNNING && s->state != ST_PAUSED) {
        log_msg("STOP %s: not running", s->name);
        return -1;
    }
    /* If paused, resume first so it can actually process SIGTERM */
    if (s->state == ST_PAUSED) kill(s->pid, SIGCONT);

    log_msg("Stopping service '%s' (pid %d) with SIGTERM", s->name, s->pid);
    kill(s->pid, SIGTERM);

    /* Give it up to 3 seconds to exit gracefully */
    int waited_ms = 0;
    int status;
    while (waited_ms < 3000) {
        pid_t r = waitpid(s->pid, &status, WNOHANG);
        if (r == s->pid) {
            s->pid = 0;
            s->state = ST_STOPPED;
            log_msg("Service '%s' stopped gracefully", s->name);
            return 0;
        }
        usleep(100000); /* 100ms */
        waited_ms += 100;
    }

    /* Didn't die - force kill */
    log_msg("Service '%s' did not stop in time, sending SIGKILL", s->name);
    kill(s->pid, SIGKILL);
    waitpid(s->pid, &status, 0);
    s->pid = 0;
    s->state = ST_STOPPED;
    return 0;
}

static int pause_service(Service *s) {
    if (s->state != ST_RUNNING) {
        log_msg("PAUSE %s: service is not running", s->name);
        return -1;
    }
    kill(s->pid, SIGSTOP);
    s->state = ST_PAUSED;
    log_msg("Service '%s' (pid %d) paused (SIGSTOP)", s->name, s->pid);
    return 0;
}

static int resume_service(Service *s) {
    if (s->state != ST_PAUSED) {
        log_msg("RESUME %s: service is not paused", s->name);
        return -1;
    }
    kill(s->pid, SIGCONT);
    s->state = ST_RUNNING;
    log_msg("Service '%s' (pid %d) resumed (SIGCONT)", s->name, s->pid);
    return 0;
}

static int restart_service(Service *s) {
    log_msg("Restarting service '%s'", s->name);
    if (s->state == ST_RUNNING || s->state == ST_PAUSED) {
        stop_service(s);
    }
    return start_service(s);
}

/* ---------------------------------------------------------------------- */
/* SIGCHLD handling / reaping                                               */
/* ---------------------------------------------------------------------- */
static void sigchld_handler(int signo) {
    (void)signo;
    g_sigchld_flag = 1;
}

static void term_handler(int signo) {
    (void)signo;
    g_shutdown_flag = 1;
}

static void reap_children(void) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        Service *s = find_by_pid(pid);
        if (!s) continue; /* unknown child, ignore */

        if (WIFEXITED(status)) {
            log_msg("Service '%s' (pid %d) exited with code %d",
                    s->name, pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            log_msg("Service '%s' (pid %d) killed by signal %d",
                    s->name, pid, WTERMSIG(status));
        }

        s->pid = 0;

        if (g_shutdown_flag) {
            /* We're shutting down; don't restart anything */
            s->state = ST_STOPPED;
            continue;
        }

        s->state = ST_FAILED;

        if (s->autorestart && s->restart_count < s->max_restarts) {
            s->restart_count++;
            log_msg("Auto-restarting service '%s' (attempt %d/%d)",
                    s->name, s->restart_count, s->max_restarts);
            start_service(s);
        } else if (s->autorestart) {
            log_msg("Service '%s' exceeded max restart attempts (%d); giving up",
                    s->name, s->max_restarts);
            s->state = ST_TERMINATED;
        } else {
            s->state = ST_STOPPED;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Status reporting                                                         */
/* ---------------------------------------------------------------------- */
static void format_status(char *buf, size_t bufsize) {
    size_t off = 0;
    off += (size_t)snprintf(buf + off, bufsize - off,
        "%-15s %-8s %-10s %-10s %-10s\n",
        "SERVICE", "PID", "STATE", "RESTARTS", "MAXRESTART");
    off += (size_t)snprintf(buf + off, bufsize - off,
        "-----------------------------------------------------------\n");

    for (int i = 0; i < service_count && off < bufsize; i++) {
        Service *s = &services[i];
        off += (size_t)snprintf(buf + off, bufsize - off,
            "%-15s %-8d %-10s %-10d %-10d\n",
            s->name,
            s->pid,
            state_name(s->state),
            s->restart_count,
            s->max_restarts);
    }
}

static void print_status_console(void) {
    char buf[4096];
    format_status(buf, sizeof(buf));
    printf("\n%s\n", buf);
}

static void write_status_file(void) {
    char buf[4096];
    format_status(buf, sizeof(buf));
    FILE *fp = fopen(STATUS_FILE_PATH, "w");
    if (fp) {
        fputs(buf, fp);
        fclose(fp);
    }
}

/* ---------------------------------------------------------------------- */
/* Control command handling (via FIFO)                                     */
/*                                                                          */
/* Commands (one per line, sent by svctl):                                 */
/*   STATUS                                                                 */
/*   START <name>                                                          */
/*   STOP <name>                                                           */
/*   RESTART <name>                                                        */
/*   PAUSE <name>                                                          */
/*   RESUME <name>                                                         */
/*   SHUTDOWN                                                               */
/* ---------------------------------------------------------------------- */
static void send_reply(const char *msg) {
    /* Opening for write will block until svctl opens its end for read.
     * We open non-blocking-ish by just trying; svctl always opens promptly
     * after sending its command, so this is safe in practice. */
    int fd = open(REPLY_FIFO_PATH, O_WRONLY);
    if (fd < 0) {
        log_msg("WARNING: could not open reply fifo: %s", strerror(errno));
        return;
    }
    ssize_t w = write(fd, msg, strlen(msg));
    (void)w;
    close(fd);
}

static void process_command(char *line) {
    /* trim */
    line[strcspn(line, "\r\n")] = '\0';
    char *cursor = line;
    while (isspace((unsigned char)*cursor)) cursor++;
    if (*cursor == '\0') return;

    char cmd[32] = {0};
    char arg[NAME_LEN] = {0};
    sscanf(cursor, "%31s %63s", cmd, arg);

    log_msg("COMMAND received: %s %s", cmd, arg[0] ? arg : "");

    char reply[4096];
    reply[0] = '\0';

    if (strcasecmp(cmd, "STATUS") == 0) {
        format_status(reply, sizeof(reply));
        write_status_file();
        send_reply(reply);
        print_status_console();
        return;
    }

    if (strcasecmp(cmd, "SHUTDOWN") == 0) {
        g_shutdown_flag = 1;
        send_reply("Shutdown initiated.\n");
        return;
    }

    Service *s = find_by_name(arg);
    if (!s) {
        snprintf(reply, sizeof(reply), "ERROR: unknown service '%s'\n", arg);
        send_reply(reply);
        return;
    }

    int rc = -1;
    if      (strcasecmp(cmd, "START")   == 0) rc = start_service(s);
    else if (strcasecmp(cmd, "STOP")    == 0) rc = stop_service(s);
    else if (strcasecmp(cmd, "RESTART") == 0) rc = restart_service(s);
    else if (strcasecmp(cmd, "PAUSE")   == 0) rc = pause_service(s);
    else if (strcasecmp(cmd, "RESUME")  == 0) rc = resume_service(s);
    else {
        snprintf(reply, sizeof(reply), "ERROR: unknown command '%s'\n", cmd);
        send_reply(reply);
        return;
    }

    snprintf(reply, sizeof(reply), "%s %s -> %s\n",
             cmd, arg, rc == 0 ? "OK" : "FAILED");
    send_reply(reply);
    write_status_file();
}

/* ---------------------------------------------------------------------- */
/* FIFO setup                                                               */
/* ---------------------------------------------------------------------- */
static void setup_fifos(void) {
    unlink(CMD_FIFO_PATH);
    unlink(REPLY_FIFO_PATH);

    if (mkfifo(CMD_FIFO_PATH, 0666) < 0) {
        fprintf(stderr, "ERROR: mkfifo(%s) failed: %s\n", CMD_FIFO_PATH, strerror(errno));
        exit(1);
    }
    if (mkfifo(REPLY_FIFO_PATH, 0666) < 0) {
        fprintf(stderr, "ERROR: mkfifo(%s) failed: %s\n", REPLY_FIFO_PATH, strerror(errno));
        exit(1);
    }

    /* Open read+write on the cmd fifo ourselves so it never sees EOF
     * even when no client currently has it open for writing. */
    cmd_fifo_fd = open(CMD_FIFO_PATH, O_RDWR | O_NONBLOCK);
    if (cmd_fifo_fd < 0) {
        fprintf(stderr, "ERROR: opening cmd fifo failed: %s\n", strerror(errno));
        exit(1);
    }
}

static void cleanup_fifos(void) {
    if (cmd_fifo_fd >= 0) close(cmd_fifo_fd);
    unlink(CMD_FIFO_PATH);
    unlink(REPLY_FIFO_PATH);
}

/* ---------------------------------------------------------------------- */
/* Shutdown all services                                                    */
/* ---------------------------------------------------------------------- */
static void shutdown_all(void) {
    log_msg("=== Supervisor shutting down: stopping all services ===");
    for (int i = 0; i < service_count; i++) {
        if (services[i].state == ST_RUNNING || services[i].state == ST_PAUSED) {
            stop_service(&services[i]);
        }
    }
    write_status_file();
    log_msg("=== All services stopped. Supervisor exiting. ===");
}

/* ---------------------------------------------------------------------- */
/* Main                                                                     */
/* ---------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    const char *config_path = (argc > 1) ? argv[1] : "services.conf";

    printf("=========================================\n");
    printf(" Linux Service & Process Supervisor\n");
    printf("=========================================\n");

    if (load_config(config_path) != 0) {
        return 1;
    }
    if (service_count == 0) {
        fprintf(stderr, "No services defined in '%s'. Exiting.\n", config_path);
        return 1;
    }

    log_msg("Loaded %d service(s) from '%s'", service_count, config_path);

    /* Install signal handlers */
    struct sigaction sa_chld, sa_term;

    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, NULL);

    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = term_handler;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);

    setup_fifos();
    log_msg("Control FIFOs ready: %s / %s", CMD_FIFO_PATH, REPLY_FIFO_PATH);
    log_msg("Use ./svctl <command> to control the supervisor.");

    /* Start all configured services */
    for (int i = 0; i < service_count; i++) {
        start_service(&services[i]);
    }
    write_status_file();

    /* Main event loop */
    char readbuf[LINE_LEN];
    char linebuf[LINE_LEN];
    size_t linelen = 0;

    while (!g_shutdown_flag) {
        if (g_sigchld_flag) {
            g_sigchld_flag = 0;
            reap_children();
            write_status_file();
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cmd_fifo_fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int ret = select(cmd_fifo_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue; /* interrupted by signal, loop again */
            log_msg("ERROR: select() failed: %s", strerror(errno));
            break;
        }

        if (ret > 0 && FD_ISSET(cmd_fifo_fd, &rfds)) {
            ssize_t n = read(cmd_fifo_fd, readbuf, sizeof(readbuf) - 1);
            if (n > 0) {
                readbuf[n] = '\0';
                for (ssize_t i = 0; i < n; i++) {
                    char c = readbuf[i];
                    if (c == '\n') {
                        linebuf[linelen] = '\0';
                        if (linelen > 0) process_command(linebuf);
                        linelen = 0;
                    } else if (linelen < sizeof(linebuf) - 1) {
                        linebuf[linelen++] = c;
                    }
                }
            }
        }

        /* re-check shutdown flag possibly set by a SHUTDOWN command */
        if (g_shutdown_flag) break;
    }

    shutdown_all();
    cleanup_fifos();
    return 0;
}
