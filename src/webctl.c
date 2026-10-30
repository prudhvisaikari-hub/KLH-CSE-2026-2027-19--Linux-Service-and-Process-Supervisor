#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>

#define PORT 8080
#define BUFFER_SIZE 65536

static const char *HTML_HEADER = 
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"    <meta charset=\"UTF-8\">\n"
"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"    <title>Supervisor Web Dashboard</title>\n"
"    <!-- Auto-refresh every 3 seconds -->\n"
"    <meta http-equiv=\"refresh\" content=\"3\">\n"
"    <style>\n"
"        :root { --bg: #0f172a; --text: #f8fafc; --card: #1e293b; --primary: #3b82f6; --success: #10b981; --danger: #ef4444; --warning: #f59e0b; --term: #64748b; --border: #334155; }\n"
"        body { font-family: system-ui, -apple-system, sans-serif; background: var(--bg); color: var(--text); padding: 2rem; margin: 0; }\n"
"        .card { background: var(--card); padding: 1.5rem; border-radius: 8px; box-shadow: 0 4px 6px rgba(0,0,0,0.2); border: 1px solid var(--border); margin-bottom: 1.5rem; }\n"
"        table { width: 100%; border-collapse: collapse; margin-top: 1rem; font-family: monospace; font-size: 0.95rem; }\n"
"        th, td { padding: 0.85rem 1rem; text-align: left; border-bottom: 1px solid var(--border); }\n"
"        th { color: #94a3b8; font-family: system-ui, sans-serif; font-size: 0.85rem; text-transform: uppercase; }\n"
"        .btn { padding: 0.45rem 0.9rem; border: none; border-radius: 4px; cursor: pointer; font-weight: bold; color: white; margin-right: 0.5rem; display: inline-block; text-decoration: none; font-size: 0.85rem; }\n"
"        .btn-primary { background: var(--primary); }\n"
"        .btn-success { background: var(--success); }\n"
"        .btn-warning { background: var(--warning); color: black; }\n"
"        .btn-danger { background: var(--danger); }\n"
"        header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 2rem; }\n"
"        .badge { padding: 0.25rem 0.55rem; border-radius: 4px; font-weight: bold; font-size: 0.85rem; }\n"
"        .badge-running { background: rgba(16, 185, 129, 0.2); color: var(--success); border: 1px solid var(--success); }\n"
"        .badge-paused { background: rgba(245, 158, 11, 0.2); color: var(--warning); border: 1px solid var(--warning); }\n"
"        .badge-stopped { background: rgba(239, 68, 68, 0.2); color: var(--danger); border: 1px solid var(--danger); }\n"
"        .badge-terminated { background: rgba(100, 116, 139, 0.2); color: var(--term); border: 1px solid var(--term); }\n"
"        .error-msg { background: rgba(239, 68, 68, 0.1); border-left: 4px solid var(--danger); padding: 1rem; margin-bottom: 1rem; }\n"
"        .section-title { font-size: 1.25rem; font-weight: bold; margin-top: 0; margin-bottom: 1rem; border-bottom: 1px solid var(--border); padding-bottom: 0.5rem; }\n"
"        .server-links { display: flex; gap: 0.75rem; flex-wrap: wrap; margin: 1rem 0; align-items: center; }\n"
"        .release-form { display: flex; gap: 0.75rem; align-items: center; margin: 1rem 0; flex-wrap: wrap; }\n"
"        .release-input { background: #0f172a; border: 1px solid var(--border); color: #f8fafc; padding: 0.5rem 0.8rem; border-radius: 4px; font-family: monospace; font-size: 0.95rem; width: 220px; }\n"
"        pre.log-box { background: #0b1120; border: 1px solid var(--border); border-radius: 6px; padding: 1rem; font-family: monospace; font-size: 0.85rem; color: #94a3b8; overflow-x: auto; line-height: 1.45; white-space: pre-wrap; margin: 0.5rem 0 0 0; }\n"
"    </style>\n"
"</head>\n"
"<body>\n"
"    <header>\n"
"        <h1>Supervisor Dashboard</h1>\n"
"        <div>\n"
"            <span style=\"margin-right: 1rem; font-family: monospace; color: #94a3b8;\">Last updated: %s</span>\n"
"            <form method=\"POST\" action=\"/action\" style=\"display: inline;\">\n"
"                <input type=\"hidden\" name=\"cmd\" value=\"SHUTDOWN\">\n"
"                <button class=\"btn btn-danger\" type=\"submit\">Shutdown All</button>\n"
"            </form>\n"
"        </div>\n"
"    </header>\n"
"    <div class=\"card\">\n"
"        <div class=\"section-title\">Managed Services</div>\n";

static const char *HTML_FOOTER = 
"</body>\n"
"</html>\n";

static void send_response(int client_fd, const char *header, const char *body) {
    char response[BUFFER_SIZE];
    int len = snprintf(response, sizeof(response), "%sContent-Length: %zu\r\n\r\n%s", header, strlen(body), body);
    if (len > 0) {
        ssize_t w = write(client_fd, response, (size_t)len);
        if (w < 0) {
            perror("write");
        }
    }
}

/* Read current version from deploy/VERSION */
static void get_current_release(char *buf, size_t buf_size) {
    FILE *fp = fopen("deploy/VERSION", "r");
    if (!fp) {
        snprintf(buf, buf_size, "unknown");
        return;
    }
    char line[128];
    if (fgets(line, sizeof(line), fp)) {
        char *start = line;
        while (*start && isspace((unsigned char)*start)) start++;
        char *end = start + strlen(start);
        while (end > start && isspace((unsigned char)*(end - 1))) end--;
        *end = '\0';
        if (*start) {
            snprintf(buf, buf_size, "%s", start);
        } else {
            snprintf(buf, buf_size, "unknown");
        }
    } else {
        snprintf(buf, buf_size, "unknown");
    }
    fclose(fp);
}

/* Read the last 8 lines of updater.log */
static void get_updater_log_tail(char *out, size_t out_size) {
    char lines[8][256];
    int count = 0;
    FILE *fp = fopen("updater.log", "r");
    if (!fp) {
        snprintf(out, out_size, "No updater log found yet.\n");
        return;
    }

    char buf[256];
    while (fgets(buf, sizeof(buf), fp)) {
        snprintf(lines[count % 8], sizeof(lines[0]), "%s", buf);
        count++;
    }
    fclose(fp);

    if (count == 0) {
        snprintf(out, out_size, "Updater log is empty.\n");
        return;
    }

    out[0] = '\0';
    int start = (count > 8) ? (count - 8) : 0;
    for (int i = start; i < count; i++) {
        strncat(out, lines[i % 8], out_size - strlen(out) - 1);
    }
}

static void handle_home(int client_fd) {
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char time_str[26];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

    char html[BUFFER_SIZE] = {0};
    snprintf(html, sizeof(html), HTML_HEADER, time_str);

    FILE *fp = popen("./svctl STATUS 2>&1", "r");
    if (!fp) {
        strcat(html, "<div class=\"error-msg\">Error: Could not execute svctl.</div>");
    } else {
        char line[256];
        int status_found = 0;

        strcat(html, "<table><thead><tr><th>Service</th><th>PID</th><th>State</th><th>Restarts</th><th>Actions</th></tr></thead><tbody>\n");

        while (fgets(line, sizeof(line), fp) != NULL) {
            char name[64], pid[16], state[32], restarts[16], max_restarts[16];
            if (sscanf(line, "%63s %15s %31s %15s %15s", name, pid, state, restarts, max_restarts) >= 4) {
                if (strcmp(name, "SERVICE") == 0) continue; /* Skip header */
                status_found = 1;

                char badge_class[32] = "badge-stopped";
                if (strcmp(state, "RUNNING") == 0) strcpy(badge_class, "badge-running");
                else if (strcmp(state, "PAUSED") == 0) strcpy(badge_class, "badge-paused");
                else if (strcmp(state, "TERMINATED") == 0) strcpy(badge_class, "badge-terminated");

                char row[2048];
                snprintf(row, sizeof(row), 
                    "<tr><td><strong>%s</strong></td><td>%s</td><td><span class=\"badge %s\">%s</span></td><td>%s/%s</td>"
                    "<td>"
                    "<form method=\"POST\" action=\"/action\" style=\"display: inline;\"><input type=\"hidden\" name=\"name\" value=\"%s\">"
                    "<input type=\"hidden\" name=\"cmd\" value=\"START\"><button class=\"btn btn-success\" type=\"submit\">Start</button></form>"
                    "<form method=\"POST\" action=\"/action\" style=\"display: inline;\"><input type=\"hidden\" name=\"name\" value=\"%s\">"
                    "<input type=\"hidden\" name=\"cmd\" value=\"STOP\"><button class=\"btn btn-danger\" type=\"submit\">Stop</button></form>"
                    "<form method=\"POST\" action=\"/action\" style=\"display: inline;\"><input type=\"hidden\" name=\"name\" value=\"%s\">"
                    "<input type=\"hidden\" name=\"cmd\" value=\"RESTART\"><button class=\"btn btn-primary\" type=\"submit\">Restart</button></form>",
                    name, pid, badge_class, state, restarts, max_restarts, name, name, name);

                strcat(html, row);

                if (strcmp(state, "PAUSED") == 0) {
                    char btn[512];
                    snprintf(btn, sizeof(btn), 
                        "<form method=\"POST\" action=\"/action\" style=\"display: inline;\"><input type=\"hidden\" name=\"name\" value=\"%s\">"
                        "<input type=\"hidden\" name=\"cmd\" value=\"RESUME\"><button class=\"btn btn-warning\" type=\"submit\">Resume</button></form>", name);
                    strcat(html, btn);
                } else {
                    char btn[512];
                    snprintf(btn, sizeof(btn), 
                        "<form method=\"POST\" action=\"/action\" style=\"display: inline;\"><input type=\"hidden\" name=\"name\" value=\"%s\">"
                        "<input type=\"hidden\" name=\"cmd\" value=\"PAUSE\"><button class=\"btn btn-warning\" type=\"submit\">Pause</button></form>", name);
                    strcat(html, btn);
                }
                
                strcat(html, "</td></tr>\n");
            }
        }
        strcat(html, "</tbody></table>\n");
        int ret = pclose(fp);
        
        if (ret < 0 || WEXITSTATUS(ret) != 0 || !status_found) {
            snprintf(html, sizeof(html), HTML_HEADER, time_str);
            strcat(html, "<div class=\"error-msg\"><strong>Daemon not running</strong><br>Could not communicate with supervisord. Make sure it is started.</div>");
        }
    }
    strcat(html, "    </div>\n"); /* Close first card */

    /* Deployment Panel */
    char current_rel[64];
    get_current_release(current_rel, sizeof(current_rel));

    char log_tail[4096];
    get_updater_log_tail(log_tail, sizeof(log_tail));

    char deploy_card[8192];
    snprintf(deploy_card, sizeof(deploy_card),
        "    <div class=\"card\">\n"
        "        <div class=\"section-title\">Deployment &amp; Rolling Updates</div>\n"
        "        <div style=\"margin: 0.5rem 0;\">\n"
        "            <strong>Current Deployed Release:</strong>\n"
        "            <span class=\"badge badge-running\" style=\"margin-left: 0.5rem; font-family: monospace; font-size: 1rem;\">%s</span>\n"
        "        </div>\n"
        "        <div class=\"server-links\">\n"
        "            <span style=\"color: #94a3b8; font-weight: bold;\">Live Services:</span>\n"
        "            <a href=\"http://localhost:9001\" target=\"_blank\" class=\"btn btn-primary\">web1 (:9001)</a>\n"
        "            <a href=\"http://localhost:9002\" target=\"_blank\" class=\"btn btn-primary\">web2 (:9002)</a>\n"
        "            <a href=\"http://localhost:9003\" target=\"_blank\" class=\"btn btn-primary\">web3 (:9003)</a>\n"
        "        </div>\n"
        "        <div style=\"margin-top: 1.25rem;\">\n"
        "            <strong>Publish release:</strong>\n"
        "            <form class=\"release-form\" method=\"POST\" action=\"/release\">\n"
        "                <input class=\"release-input\" type=\"text\" name=\"version\" placeholder=\"e.g. 2.0 or broken-3.0\" required>\n"
        "                <button class=\"btn btn-success\" type=\"submit\">Publish release</button>\n"
        "                <span style=\"color: #94a3b8; font-size: 0.8rem;\">(letters, digits, '.', '_', '-' only; 1-32 chars)</span>\n"
        "            </form>\n"
        "        </div>\n"
        "        <div style=\"margin-top: 1.5rem;\">\n"
        "            <div style=\"color: #94a3b8; font-weight: bold; margin-bottom: 0.25rem;\">Updater Log (Last 8 lines):</div>\n"
        "            <pre class=\"log-box\">%s</pre>\n"
        "        </div>\n"
        "    </div>\n",
        current_rel, log_tail);

    strcat(html, deploy_card);
    strcat(html, HTML_FOOTER);
    send_response(client_fd, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n", html);
}

static void handle_action(int client_fd, const char *body) {
    char cmd[64] = {0};
    char name[64] = {0};
    
    const char *c = strstr(body, "cmd=");
    if (c) sscanf(c + 4, "%63[^& \r\n]", cmd);
    
    const char *n = strstr(body, "name=");
    if (n) sscanf(n + 5, "%63[^& \r\n]", name);
    
    if (strlen(cmd) > 0) {
        char exec_cmd[256];
        if (strlen(name) > 0) {
            snprintf(exec_cmd, sizeof(exec_cmd), "./svctl %s %s > /dev/null 2>&1", cmd, name);
        } else {
            snprintf(exec_cmd, sizeof(exec_cmd), "./svctl %s > /dev/null 2>&1", cmd);
        }
        int status = system(exec_cmd);
        if (status < 0) {
            perror("system");
        }
    }
    
    /* Redirect back to home */
    const char *redirect = "HTTP/1.1 303 See Other\r\nLocation: /\r\nContent-Length: 0\r\n\r\n";
    if (write(client_fd, redirect, strlen(redirect)) < 0) {
        perror("write");
    }
}

/* Validate version: letters, digits, '.', '_', '-', 1 to 32 characters */
static int validate_version(const char *ver) {
    if (!ver) return 0;
    size_t len = strlen(ver);
    if (len < 1 || len > 32) return 0;
    for (size_t i = 0; i < len; i++) {
        char ch = ver[i];
        if (!isalnum((unsigned char)ch) && ch != '.' && ch != '_' && ch != '-') {
            return 0;
        }
    }
    return 1;
}

static void handle_release(int client_fd, const char *body) {
    const char *v = strstr(body, "version=");
    if (!v) {
        send_response(client_fd,
                      "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n",
                      "ERROR: Missing 'version' parameter.\n");
        return;
    }

    v += 8; /* Skip "version=" */
    char raw_ver[128] = {0};
    size_t idx = 0;
    while (*v && *v != '&' && *v != '\r' && *v != '\n' && idx < sizeof(raw_ver) - 1) {
        raw_ver[idx++] = *v++;
    }
    raw_ver[idx] = '\0';

    if (!validate_version(raw_ver)) {
        send_response(client_fd,
                      "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n",
                      "ERROR: Invalid version format. Version must be 1-32 characters "
                      "and contain only letters, digits, '.', '_', and '-'.\n");
        return;
    }

    FILE *fp = fopen("deploy/VERSION", "w");
    if (!fp) {
        send_response(client_fd,
                      "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n",
                      "ERROR: Failed to write to deploy/VERSION.\n");
        return;
    }
    fprintf(fp, "%s\n", raw_ver);
    fflush(fp);
    fclose(fp);

    /* Redirect back to home */
    const char *redirect = "HTTP/1.1 303 See Other\r\nLocation: /\r\nContent-Length: 0\r\n\r\n";
    if (write(client_fd, redirect, strlen(redirect)) < 0) {
        perror("write");
    }
}

static void handle_client(int client_fd) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        close(client_fd);
        return;
    }
    buffer[bytes_read] = '\0';

    char method[16] = {0};
    char path[256] = {0};
    char version[16] = {0};
    if (sscanf(buffer, "%15s %255s %15s", method, path, version) < 2) {
        send_response(client_fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n", "Bad Request\n");
        close(client_fd);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        handle_home(client_fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/action") == 0) {
        char *body = strstr(buffer, "\r\n\r\n");
        if (body) body += 4; else body = "";
        handle_action(client_fd, body);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/release") == 0) {
        char *body = strstr(buffer, "\r\n\r\n");
        if (body) body += 4; else body = "";
        handle_release(client_fd, body);
    } else {
        send_response(client_fd, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n", "Not Found\n");
    }

    close(client_fd);
}

int main(void) {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Webctl running at http://localhost:%d\n", PORT);
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
        
        handle_client(client_fd);
    }

    close(server_fd);
    return 0;
}
