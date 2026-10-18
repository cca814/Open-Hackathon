#include "tcp.h"

int send_connection_data(int fd, const void *data, size_t len) {
  const char *ptr = (const char *)data;
  size_t total_sent = 0;

  while (total_sent < len) {
    ssize_t bytes_sent =
        send(fd, ptr + total_sent, len - total_sent, MSG_NOSIGNAL);

    if (bytes_sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }

    if (bytes_sent == 0) {
      return -1;
    }

    total_sent += (size_t)bytes_sent;
  }

  return 0;
}

int receive_connection_data(int fd, void *data, size_t len) {
  char *p = data;
  size_t total_recv = 0;

  while (total_recv < len) {
    ssize_t bytes_recv = recv(fd, p + total_recv, len - total_recv, 0);

    if (bytes_recv < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }

    if (bytes_recv == 0) {
      return -1;
    }

    total_recv += (size_t)bytes_recv;
  }

  return 0;
}

int bind_server(const char *port) {
  struct addrinfo hints, *res;
  int listen_fd = -1, conn_fd, on = 1, rc;
  memset(&hints, 0, sizeof(hints));

  hints.ai_family = AF_INET;       // IPV4
  hints.ai_socktype = SOCK_STREAM; // TCP
  hints.ai_flags = AI_PASSIVE;     // bind on 0.0.0.0

  rc = getaddrinfo(NULL, port, &hints, &res);

  if (rc) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
    return -1;
  }

  listen_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

  if (listen_fd >= 0) {
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (bind(listen_fd, res->ai_addr, res->ai_addrlen) < 0 ||
        listen(listen_fd, 1) < 0) {
      close(listen_fd);
      listen_fd = -1;
    }
  }
  freeaddrinfo(res);

  if (listen_fd < 0) {
    return -1;
  }

  printf("[Server] Waiting for IPv4 client on port %s ...\n", port);
  conn_fd = accept(listen_fd, NULL, NULL);
  close(listen_fd);

  return conn_fd;
}

int connect_server(const char *host, const char *port) {
  struct addrinfo hints, *res, *rp;
  int fd = -1, rc, attempt;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  rc = getaddrinfo(host, port, &hints, &res);
  if (rc) {
    fprintf(stderr, "getaddrinfo %s: %s\n", host, gai_strerror(rc));
    return -1;
  }

  for (attempt = 0; attempt < 10 && fd < 0; attempt++) {
    if (attempt > 0)
      sleep(1);
    for (rp = res; rp; rp = rp->ai_next) {
      fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (fd < 0)
        continue;
      if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
        break;
      close(fd);
      fd = -1;
    }
  }
  freeaddrinfo(res);

  if (fd < 0)
    fprintf(stderr, "[Client] Cannot connect to %s:%s\n", host, port);
  return fd;
}
