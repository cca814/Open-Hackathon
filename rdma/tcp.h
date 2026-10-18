#ifndef _TCP_H_
#define _TCP_H_

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int send_connection_data(int fd, const void *data, size_t len);
int receive_connection_data(int fd, void *data, size_t len);
int bind_server(const char *port);
int connect_server(const char *host, const char *port);

#endif