CC = gcc
CFLAGS = -O2 -Wall -Wextra

LDLIBS = -libverbs

DEPS = common.c tcp.c common.h tcp.h

INTF ?= $(shell ip -brief link show | awk '$$1 != "lo" { print $$1; exit }')

.PHONY: all env

all: client server

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