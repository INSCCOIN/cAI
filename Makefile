CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

cAI: cai.c
	$(CC) $(CFLAGS) -o cAI cai.c

clean:
	rm -f cAI
