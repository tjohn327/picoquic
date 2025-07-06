# Makefile for deadline real-world tests

CC = gcc
CFLAGS = -Wall -O2 -I../loglib -I../picoquic -I../picohttp -I../_deps/picotls-src/include
PTLS_LIBS = ../_deps/picotls-build/libpicotls-core.a ../_deps/picotls-build/libpicotls-openssl.a ../_deps/picotls-build/libpicotls-minicrypto.a ../_deps/picotls-build/libpicotls-fusion.a
LDFLAGS = -L.. -lpicoquic-log -lpicoquic-core $(PTLS_LIBS) -lssl -lcrypto -lpthread -lm -ldl

TARGETS = deadline_server deadline_client deadline_client_fixed deadline_test_verify deadline_frame_monitor

all: $(TARGETS)

deadline_server: deadline_server.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

deadline_client: deadline_client.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

deadline_client_fixed: deadline_client_fixed.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

deadline_test_verify: deadline_test_verify.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

deadline_frame_monitor: deadline_frame_monitor.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean