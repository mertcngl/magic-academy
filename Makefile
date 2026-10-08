# Makefile - CSE 344 HW6 "Magic Academy"
# Builds all three programs with zero -Wall -Wextra warnings on Debian 12 (gcc).

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11
TARGETS = hogwarts wizard professor

.PHONY: all clean

all: $(TARGETS)

hogwarts: hogwarts.c shared.h
	$(CC) $(CFLAGS) -o $@ hogwarts.c

wizard: wizard.c shared.h
	$(CC) $(CFLAGS) -o $@ wizard.c

professor: professor.c shared.h
	$(CC) $(CFLAGS) -o $@ professor.c

clean:
	rm -f $(TARGETS) *.o
