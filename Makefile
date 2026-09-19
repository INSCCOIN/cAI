CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LIBS = -lcurl

cAI: cai.c
	$(CC) $(CFLAGS) -o cAI cai.c $(LIBS)

clean:
	rm -f cAI
