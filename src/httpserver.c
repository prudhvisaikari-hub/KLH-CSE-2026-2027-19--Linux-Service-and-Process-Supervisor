/*
 * httpserver.c - Lightweight, single-threaded HTTP status server for Linux services.
 *
 * Usage:
 *   ./httpserver <name> <port> <version_file>
 *
 * Endpoints:
 *   GET /health -> HTTP 200 "OK name=<name> version=<version> pid=<pid>\n"
 *                  or HTTP 503 "UNHEALTHY name=<name> version=<version> pid=<pid>\n"
 *                  if version string contains "broken".
 *   GET /       -> Dark-themed HTML dashboard showing name, version, health, port,
 *                  pid, uptime, auto-refreshing every 2s.
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
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static char   g_name[64];
static int    g_port;
static char   g_version[64];
static int    g_is_broken = 0;
static time_t g_start_time;
static pid_t  g_pid;

/* Helper function to perform robust full writes and check return values */
static ssize_t write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        p += n;
        remaining -= (size_t)n;
    }
    return (ssize_t)(len - remaining);
}

/* Send a formatted HTTP response with headers and body */
static void send_response(int fd, const char *status_line, const char *content_type, const char *body) {
    char header[512];
    size_t body_len = body ? strlen(body) : 0;
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.0 %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n\r\n",
                        status_line, content_type, body_len);
    if (hlen > 0) {
        if (write_all(fd, header, (size_t)hlen) < 0) {
            /* Client disconnected or write error */
            return;
        }
    }
    if (body_len > 0) {
        if (write_all(fd, body, body_len) < 0) {
            /* Client disconnected or write error */
            return;
        }
    }
}

/* Read release version from version_file ONCE at startup */
static void load_version(const char *version_file) {
    FILE *fp = fopen(version_file, "r");
    if (!fp) {
        strncpy(g_version, "unknown", sizeof(g_version) - 1);
        g_version[sizeof(g_version) - 1] = '\0';
        return;
    }

    char buf[128];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        strncpy(g_version, "unknown", sizeof(g_version) - 1);
        g_version[sizeof(g_version) - 1] = '\0';
        return;
    }
    fclose(fp);

    /* Trim leading and trailing whitespace */
    char *start = buf;
    while (*start && isspace((unsigned char)*start)) start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';

    if (*start == '\0') {
        strncpy(g_version, "unknown", sizeof(g_version) - 1);
    } else {
        strncpy(g_version, start, sizeof(g_version) - 1);
    }
    g_version[sizeof(g_version) - 1] = '\0';

    if (strstr(g_version, "broken") != NULL) {
        g_is_broken = 1;
    }
}

/* Serve HTML status dashboard */
static void handle_dashboard(int fd) {
    long uptime = (long)(time(NULL) - g_start_time);
    const char *status_badge = g_is_broken ?
        "<span class=\"badge badge-unhealthy\">UNHEALTHY</span>" :
        "<span class=\"badge badge-healthy\">HEALTHY</span>";

    char body[4096];
    snprintf(body, sizeof(body),
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "    <meta charset=\"UTF-8\">\n"
        "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "    <title>%s - Service Status</title>\n"
        "    <meta http-equiv=\"refresh\" content=\"2\">\n"
        "    <style>\n"
        "        :root {\n"
        "            --bg: #0f172a; --card: #1e293b; --text: #f8fafc; --muted: #94a3b8;\n"
        "            --border: #334155; --success: #10b981; --danger: #ef4444;\n"
        "        }\n"
        "        * { box-sizing: border-box; }\n"
        "        body {\n"
        "            font-family: system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;\n"
        "            background: var(--bg); color: var(--text); padding: 2rem; margin: 0;\n"
        "            display: flex; justify-content: center; align-items: center; min-height: 85vh;\n"
        "        }\n"
        "        .card {\n"
        "            background: var(--card); border: 1px solid var(--border); border-radius: 12px;\n"
        "            padding: 2rem; width: 100%%; max-width: 480px; box-shadow: 0 10px 25px rgba(0,0,0,0.35);\n"
        "        }\n"
        "        h1 {\n"
        "            margin-top: 0; font-size: 1.5rem; display: flex; align-items: center;\n"
        "            justify-content: space-between; border-bottom: 1px solid var(--border); padding-bottom: 1rem;\n"
        "        }\n"
        "        .badge {\n"
        "            font-size: 0.8rem; padding: 0.3rem 0.65rem; border-radius: 6px; font-weight: 700; letter-spacing: 0.05em;\n"
        "        }\n"
        "        .badge-healthy { background: rgba(16,185,129,0.2); color: var(--success); border: 1px solid var(--success); }\n"
        "        .badge-unhealthy { background: rgba(239,68,68,0.2); color: var(--danger); border: 1px solid var(--danger); }\n"
        "        .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; margin-top: 1.5rem; font-family: monospace; }\n"
        "        .metric { background: #0b1120; padding: 0.85rem 1rem; border-radius: 8px; border: 1px solid var(--border); }\n"
        "        .label { color: var(--muted); font-size: 0.75rem; text-transform: uppercase; margin-bottom: 0.35rem; font-family: system-ui, sans-serif; }\n"
        "        .value { font-weight: 700; font-size: 1.15rem; color: #ffffff; }\n"
        "        .footer { margin-top: 1.5rem; text-align: center; color: var(--muted); font-size: 0.8rem; border-top: 1px solid var(--border); padding-top: 1rem; }\n"
        "    </style>\n"
        "</head>\n"
        "<body>\n"
        "    <div class=\"card\">\n"
        "        <h1><span>%s</span> %s</h1>\n"
        "        <div class=\"grid\">\n"
        "            <div class=\"metric\"><div class=\"label\">Version</div><div class=\"value\">%s</div></div>\n"
        "            <div class=\"metric\"><div class=\"label\">Port</div><div class=\"value\">%d</div></div>\n"
        "            <div class=\"metric\"><div class=\"label\">PID</div><div class=\"value\">%d</div></div>\n"
        "            <div class=\"metric\"><div class=\"label\">Uptime</div><div class=\"value\">%ld s</div></div>\n"
        "        </div>\n"
        "        <div class=\"footer\">Auto-refreshes every 2s &bull; Supervised HTTP Service</div>\n"
        "    </div>\n"
        "</body>\n"
        "</html>\n",
        g_name, g_name, status_badge, g_version, g_port, (int)g_pid, uptime);

    send_response(fd, "200 OK", "text/html", body);
}

/* Serve health check endpoint */
static void handle_health(int fd) {
    char body[256];
    if (g_is_broken) {
        snprintf(body, sizeof(body), "UNHEALTHY name=%s version=%s pid=%d\n", g_name, g_version, (int)g_pid);
        send_response(fd, "503 Service Unavailable", "text/plain", body);
    } else {
        snprintf(body, sizeof(body), "OK name=%s version=%s pid=%d\n", g_name, g_version, (int)g_pid);
        send_response(fd, "200 OK", "text/plain", body);
    }
}

/* Parse incoming HTTP request */
static void handle_client(int fd) {
    char req[2048];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) {
        return;
    }
    req[n] = '\0';

    char method[16] = {0};
    char path[256] = {0};
    if (sscanf(req, "%15s %255s", method, path) < 2) {
        send_response(fd, "400 Bad Request", "text/plain", "Bad Request\n");
        return;
    }

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/health") == 0) {
            handle_health(fd);
        } else if (strcmp(path, "/") == 0) {
            handle_dashboard(fd);
        } else {
            send_response(fd, "404 Not Found", "text/plain", "Not Found\n");
        }
    } else {
        send_response(fd, "405 Method Not Allowed", "text/plain", "Method Not Allowed\n");
    }
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <name> <port> <version_file>\n", argv[0]);
        return 1;
    }

    strncpy(g_name, argv[1], sizeof(g_name) - 1);
    g_name[sizeof(g_name) - 1] = '\0';
    g_port = atoi(argv[2]);
    const char *version_file = argv[3];

    g_pid = getpid();
    g_start_time = time(NULL);

    load_version(version_file);

    /* Ignore SIGPIPE so disconnected clients do not kill the server */
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)g_port);

    int bound = 0;
    for (int retry = 0; retry < 5; retry++) {
        if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            bound = 1;
            break;
        }
        usleep(100000); /* 100ms */
    }
    if (!bound) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 64) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    /* Print exactly one startup line to stdout and flush it */
    printf("[%s] listening on port %d (version: %s, pid: %d)\n",
           g_name, g_port, g_version, (int)g_pid);
    fflush(stdout);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        /* 2-second receive timeout per client */
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            /* non-fatal */
        }

        handle_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
