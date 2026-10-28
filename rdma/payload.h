#ifndef _PAYLOAD_H
#define _PAYLOAD_H

#include <stdint.h>
#include <time.h>

// Test message: an 8-byte big-endian sequence number, then, when the message is
// at least PAYLOAD_TS_MIN_LEN bytes, an 8-byte big-endian send time (see
// payload_stamp), then bytes that depend on the sequence number and the offset,
// so a sink can check that a message arrived intact and in order.
#define PAYLOAD_MIN_LEN 8
#define PAYLOAD_TS_MIN_LEN 16

// Nanoseconds on CLOCK_MONOTONIC. Two processes can compare these only if they
// run on the same machine.
static inline uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void payload_put64(uint8_t *buf, uint64_t value) {
  for (int i = 0; i < 8; i++) {
    buf[i] = (uint8_t)(value >> (56 - 8 * i));
  }
}

static inline uint64_t payload_get64(const uint8_t *buf) {
  uint64_t value = 0;
  for (int i = 0; i < 8; i++) {
    value = (value << 8) | buf[i];
  }
  return value;
}

// Offset of the first pattern byte: after the sequence number, and after the
// send time when the message is long enough to hold one.
static inline uint32_t payload_pattern_start(uint32_t len) {
  return len >= PAYLOAD_TS_MIN_LEN ? 16 : 8;
}

static inline void payload_fill(uint8_t *buf, uint32_t len, uint64_t seq) {
  payload_put64(buf, seq);
  for (uint32_t i = 8; i < payload_pattern_start(len) && i < len; i++) {
    buf[i] = 0;
  }
  for (uint32_t i = payload_pattern_start(len); i < len; i++) {
    buf[i] = (uint8_t)(seq + i);
  }
}

// Writes the send time, taken just before the send is posted. A message shorter
// than PAYLOAD_TS_MIN_LEN has no room for it.
static inline void payload_stamp(uint8_t *buf, uint32_t len, uint64_t ns) {
  if (len >= PAYLOAD_TS_MIN_LEN) {
    payload_put64(buf + 8, ns);
  }
}

// Returns 0 and stores the send time, or -1 if the message has none.
static inline int payload_sent_ns(const uint8_t *buf, uint32_t len, uint64_t *ns) {
  if (len < PAYLOAD_TS_MIN_LEN) {
    return -1;
  }
  *ns = payload_get64(buf + 8);
  return *ns != 0 ? 0 : -1;
}

// Returns 0 and stores the sequence number if the message is intact. The send
// time is not checked: it differs on every message.
static inline int payload_check(const uint8_t *buf, uint32_t len, uint64_t *seq) {
  if (len < PAYLOAD_MIN_LEN) {
    return -1;
  }
  uint64_t value = payload_get64(buf);
  for (uint32_t i = payload_pattern_start(len); i < len; i++) {
    if (buf[i] != (uint8_t)(value + i)) {
      return -1;
    }
  }
  *seq = value;
  return 0;
}

#endif
