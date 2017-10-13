CC = gcc
CFLAGS = -g -O2 -Wall
LDFLAGS = -lpthread

.PHONY: clean

all: dumb_acceptor dumb_connector

dumb_acceptor: dumb_acceptor.c util.o
	$(CC) $(CFLAGS) -o $@ $< util.o $(LDFLAGS)

dumb_connector: dumb_connector.c util.o
	$(CC) $(CFLAGS) -o $@ $< util.o $(LDFLAGS)

util.o: util.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f dumb_acceptor dumb_connector util.o
