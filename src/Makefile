CC = gcc
CFLAGS = -Wall -Wextra -g -O2
TARGETS = supervisord svctl

all: $(TARGETS)

supervisord: supervisor.c
	$(CC) $(CFLAGS) -o supervisord supervisor.c

svctl: svctl.c
	$(CC) $(CFLAGS) -o svctl svctl.c

clean:
	rm -f $(TARGETS) supervisor.log supervisor_status.txt
	rm -f /tmp/supervisor_cmd.fifo /tmp/supervisor_reply.fifo

.PHONY: all clean
