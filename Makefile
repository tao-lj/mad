CC ?= cc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -pedantic

all: mad

mad: mad.c
	$(CC) $(CFLAGS) mad.c -o mad

clean:
	rm -f mad
