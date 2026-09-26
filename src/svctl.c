/*
 * svctl.c - Control client for the supervisor daemon.
 *
 * Sends a command line to the supervisor's command FIFO and prints
 * whatever reply the supervisor sends back on the reply FIFO.
 *
 * Usage:
 *   ./svctl STATUS
 *   ./svctl START   <service>
 *   ./svctl STOP    <service>
 *   ./svctl RESTART <service>
 *   ./svctl PAUSE   <service>
 *   ./svctl RESUME  <service>
 *   ./svctl SHUTDOWN
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define CMD_FIFO_PATH    "/tmp/supervisor_cmd.fifo"
#define REPLY_FIFO_PATH  "/tmp/supervisor_reply.fifo"

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <COMMAND> [service_name]\n"
        "Commands:\n"
        "  STATUS                 Show status of all services\n"
        "  START   <name>         Start a service\n"
        "  STOP    <name>         Stop a service\n"
        "  RESTART <name>         Restart a service\n"
        "  PAUSE   <name>         Pause (SIGSTOP) a service\n"
        "  RESUME  <name>         Resume (SIGCONT) a service\n"
        "  SHUTDOWN                Stop the supervisor and all services\n",
        prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    char line[256];
    if (argc >= 3) {
        snprintf(line, sizeof(line), "%s %s\n", argv[1], argv[2]);
    } else {
        snprintf(line, sizeof(line), "%s\n", argv[1]);
    }

    int fd = open(CMD_FIFO_PATH, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR: cannot open command fifo (%s). "
                        "Is the supervisor running? (%s)\n",
                        CMD_FIFO_PATH, strerror(errno));
        return 1;
    }

    ssize_t w = write(fd, line, strlen(line));
    close(fd);
    if (w < 0) {
        fprintf(stderr, "ERROR: failed to write command: %s\n", strerror(errno));
        return 1;
    }

    /* Wait for the supervisor's reply. This blocks until the supervisor
     * opens the reply fifo for writing - which it does right after
     * processing our command. */
    int rfd = open(REPLY_FIFO_PATH, O_RDONLY);
    if (rfd < 0) {
        fprintf(stderr, "ERROR: cannot open reply fifo: %s\n", strerror(errno));
        return 1;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(rfd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        fputs(buf, stdout);
    }
    close(rfd);

    return 0;
}
