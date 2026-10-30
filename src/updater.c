/*
 * updater.c - Automated rolling update & rollback agent for supervised services.
 *
 * Usage:
 *   ./updater <version_file> <name:port> [<name:port> ...]
 *
 * Runs continuously, monitoring <version_file> every 2 seconds.
 * On change:
 *   - Performs a rolling restart across targets one by one.
 *   - Verifies HTTP 200 and version string via TCP health check.
 *   - If any server fails: restores previous version in file,
 *     rolls back all touched servers, and leaves untouched servers alone.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_TARGETS 32
#define LOG_FILE    "updater.log"

typedef struct {
    char name[64];
    int  port;
} Target;

static Target targets[MAX_TARGETS];
static int    num_targets = 0;
static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* Timestamped logging to stdout and updater.log */
static void log_updater(const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tmv);

    va_list ap;

    /* stdout */
    printf("[%s] ", tbuf);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);

    /* updater.log */
    FILE *fp = fopen(LOG_FILE, "a");
    if (fp) {
        fprintf(fp, "[%s] ", tbuf);
        va_start(ap, fmt);
        vfprintf(fp, fmt, ap);
        va_end(ap);
        fprintf(fp, "\n");
        fflush(fp);
        fclose(fp);
    }
}

/* Validate server names (letters, digits, _ and - only) */
static int is_valid_name(const char *name) {
    if (!name || !*name) return 0;
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
            return 0;
        }
    }
    return 1;
}

/* Read release version from file, trimming whitespace. Returns 1 on success, 0 on empty/error */
static int read_version(const char *path, char *out, size_t out_size) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char buf[128];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    char *start = buf;
    while (*start && isspace((unsigned char)*start)) start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';

    if (*start == '\0') return 0;

    strncpy(out, start, out_size - 1);
    out[out_size - 1] = '\0';
    return 1;
}

/* Write version string back to file */
static void write_version_file(const char *path, const char *ver) {
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%s\n", ver);
        fflush(fp);
        fclose(fp);
    }
}

/* Run ./svctl RESTART <name> via popen and check reply */
static int restart_server(const char *name) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "./svctl RESTART %s 2>&1", name);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;

    char reply[512];
    int found_ok = 0;
    while (fgets(reply, sizeof(reply), fp) != NULL) {
        if (strstr(reply, "-> OK") != NULL) {
            found_ok = 1;
        }
    }
    int status = pclose(fp);
    if (status < 0 || WEXITSTATUS(status) != 0) {
        found_ok = 0;
    }
    return found_ok;
}

/* Raw socket HTTP health check to 127.0.0.1:<port> */
static int check_health_once(int port, const char *expected_version) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return 0;
    }

    const char *req = "GET /health HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    ssize_t w = write(sock, req, strlen(req));
    if (w < 0 || (size_t)w != strlen(req)) {
        close(sock);
        return 0;
    }

    char buf[1024];
    ssize_t n = read(sock, buf, sizeof(buf) - 1);
    close(sock);
    if (n <= 0) return 0;
    buf[n] = '\0';

    /* Must be HTTP 200 */
    if (strstr(buf, "HTTP/1.0 200") == NULL &&
        strstr(buf, "HTTP/1.1 200") == NULL &&
        strstr(buf, "200 OK") == NULL) {
        return 0;
    }

    /* Must contain "version=<expected_version> " */
    char ver_str[128];
    snprintf(ver_str, sizeof(ver_str), "version=%s ", expected_version);
    if (strstr(buf, ver_str) == NULL) {
        return 0;
    }

    return 1;
}

/* Retry health check up to 10 times, 0.5s apart */
static int verify_server_health(int port, const char *expected_version) {
    for (int attempt = 1; attempt <= 10; attempt++) {
        if (check_health_once(port, expected_version)) {
            return 1;
        }
        if (attempt < 10) {
            usleep(500000); /* 0.5 seconds */
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <version_file> <name:port> [<name:port> ...]\n", argv[0]);
        return 1;
    }

    const char *version_file = argv[1];

    for (int i = 2; i < argc; i++) {
        if (num_targets >= MAX_TARGETS) {
            fprintf(stderr, "ERROR: maximum number of targets exceeded\n");
            return 1;
        }

        char *colon = strchr(argv[i], ':');
        if (!colon) {
            fprintf(stderr, "ERROR: invalid target format '%s' (expected name:port)\n", argv[i]);
            return 1;
        }

        size_t nlen = (size_t)(colon - argv[i]);
        if (nlen == 0 || nlen >= sizeof(targets[num_targets].name)) {
            fprintf(stderr, "ERROR: invalid service name in '%s'\n", argv[i]);
            return 1;
        }

        strncpy(targets[num_targets].name, argv[i], nlen);
        targets[num_targets].name[nlen] = '\0';

        if (!is_valid_name(targets[num_targets].name)) {
            fprintf(stderr, "ERROR: invalid characters in service name '%s'\n", targets[num_targets].name);
            return 1;
        }

        int port = atoi(colon + 1);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "ERROR: invalid port in '%s'\n", argv[i]);
            return 1;
        }
        targets[num_targets].port = port;
        num_targets++;
    }

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    char current_version[64] = {0};
    while (!g_stop && !read_version(version_file, current_version, sizeof(current_version))) {
        sleep(2);
    }
    if (g_stop) return 0;

    log_updater("updater started. Monitored version file: '%s'. Initial version: %s",
                version_file, current_version);

    while (!g_stop) {
        sleep(2);
        if (g_stop) break;

        char new_version[64] = {0};
        if (!read_version(version_file, new_version, sizeof(new_version))) {
            /* Ignore empty or unreadable reads (file may be mid-write) */
            continue;
        }

        if (strcmp(new_version, current_version) == 0) {
            continue;
        }

        log_updater("NEW RELEASE detected: %s -> %s", current_version, new_version);

        int rollout_failed = 0;
        int touched_count = 0;

        for (int i = 0; i < num_targets; i++) {
            log_updater("[%d/%d] restarting '%s'", i + 1, num_targets, targets[i].name);
            touched_count = i + 1;

            if (!restart_server(targets[i].name)) {
                log_updater("HEALTH CHECK FAILED on '%s' (restart command failed)", targets[i].name);
                rollout_failed = 1;
                break;
            }

            if (!verify_server_health(targets[i].port, new_version)) {
                log_updater("HEALTH CHECK FAILED on '%s' for version %s", targets[i].name, new_version);
                rollout_failed = 1;
                break;
            }

            log_updater("'%s' is HEALTHY on version %s", targets[i].name, new_version);
        }

        if (rollout_failed) {
            log_updater("Initiating ROLLBACK to version %s...", current_version);
            write_version_file(version_file, current_version);

            for (int j = 0; j < touched_count; j++) {
                log_updater("[rollback %d/%d] restarting '%s'", j + 1, touched_count, targets[j].name);
                restart_server(targets[j].name);
                if (verify_server_health(targets[j].port, current_version)) {
                    log_updater("'%s' is HEALTHY on version %s (rollback)", targets[j].name, current_version);
                } else {
                    log_updater("HEALTH CHECK FAILED on '%s' during rollback (version %s)", targets[j].name, current_version);
                }
            }
            log_updater("ROLLBACK COMPLETE: restored %s", current_version);
        } else {
            log_updater("ROLLOUT COMPLETE: %s", new_version);
            strncpy(current_version, new_version, sizeof(current_version) - 1);
            current_version[sizeof(current_version) - 1] = '\0';
        }
    }

    return 0;
}
