CC = gcc
CFLAGS = -O2 -Wall -Wextra

LDLIBS = -libverbs

DEPS = common.c tcp.c common.h tcp.h

INTF ?= $(shell ip -brief link show | awk '$$1 != "lo" { print $$1; exit }')

.PHONY: all env

all: client server relay sink source

env:
	sudo apt install -y \
    rdma-core \
    ibverbs-providers \
    ibverbs-utils \
    libibverbs-dev \
    librdmacm-dev \
    perftest \
    iproute2 \
    build-essential
	sudo modprobe rdma_rxe
	sudo rdma link add rxe0 type rxe netdev $(INTF)

client: client.c $(DEPS)
	$(CC) $(CFLAGS) client.c common.c tcp.c -o $@ $(LDLIBS)

server: server.c $(DEPS)
	$(CC) $(CFLAGS) server.c common.c tcp.c -o $@ $(LDLIBS)

# DPU relay (see RELAY.md): relay forwards source's messages to every sink.
RELAY_DEPS = qp.c qp.h payload.h $(DEPS)

relay: relay.c $(RELAY_DEPS)
	$(CC) $(CFLAGS) relay.c qp.c common.c tcp.c -o $@ $(LDLIBS)

sink: sink.c $(RELAY_DEPS)
	$(CC) $(CFLAGS) sink.c qp.c common.c tcp.c -o $@ $(LDLIBS)

source: source.c $(RELAY_DEPS)
	$(CC) $(CFLAGS) source.c qp.c common.c tcp.c -o $@ $(LDLIBS)