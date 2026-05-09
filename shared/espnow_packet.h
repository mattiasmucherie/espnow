#pragma once
#include <stdint.h>

// Packet types shared between bridge and senders.

typedef struct {
  uint32_t counter;
} test_payload_t;
