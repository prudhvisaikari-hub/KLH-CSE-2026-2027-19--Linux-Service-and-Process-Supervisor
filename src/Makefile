CC = gcc
CFLAGS = -Wall -Wextra -g -O2
TARGETS = supervisord svctl webctl httpserver updater

all: $(TARGETS)

supervisord: supervisor.c
	$(CC) $(CFLAGS) -o supervisord supervisor.c

svctl: svctl.c
	$(CC) $(CFLAGS) -o svctl svctl.c

webctl: webctl.c
	$(CC) $(CFLAGS) -o webctl webctl.c

httpserver: httpserver.c
	$(CC) $(CFLAGS) -o httpserver httpserver.c

updater: updater.c
	$(CC) $(CFLAGS) -o updater updater.c

clean:
	rm -f $(TARGETS) supervisor.log supervisor_status.txt updater.log
	rm -f /tmp/supervisor_cmd.fifo /tmp/supervisor_reply.fifo

.PHONY: all clean
