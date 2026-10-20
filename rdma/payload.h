#ifndef _PAYLOAD_H
#define _PAYLOAD_H

#include <stdint.h>

// Test message: an 8-byte big-endian sequence number, then bytes that depend on
// the sequence number and the offset, so a sink can check that a message
// arrived intact and in order.
#define PAYLOAD_MIN_LEN 8

static inline void payload_fill(uint8_t *buf, uint32_t len, uint64_t seq) {
  for (int i = 0; i < 8; i++) {
    buf[i] = (uint8_t)(seq >> (56 - 8 * i));
  }
  for (uint32_t i = 8; i < len; i++) {
    buf[i] = (uint8_t)(seq + i);
  }
}

// Returns 0 and stores the sequence number if the message is intact.
static inline int payload_check(const uint8_t *buf, uint32_t len, uint64_t *seq) {
  if (len < PAYLOAD_MIN_LEN) {
    return -1;
  }
  uint64_t value = 0;
  for (int i = 0; i < 8; i++) {
    value = (value << 8) | buf[i];
  }
  for (uint32_t i = 8; i < len; i++) {
    if (buf[i] != (uint8_t)(value + i)) {
      return -1;
    }
  }
  *seq = value;
  return 0;
}

#endif
