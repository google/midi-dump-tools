packages=alsa gthread-2.0 glib-2.0 json-glib-1.0 gio-unix-2.0

CC=gcc
CFLAGS=-g -O2 -Wall -D_FORTIFY_SOURCE=2 -std=c99 $(shell pkg-config $(packages) --cflags) -D_POSIX_C_SOURCE=200112
LDFLAGS=-g $(shell pkg-config $(packages) --libs)

aseq2json : aseq2json.o

clean:
	$(RM) aseq2json *.o
