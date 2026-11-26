#pragma once
#include "MatWire.h"
#include <stdbool.h>

// Independent, negotiated messages on the reliable encrypted control channel.
#define MAT_BACKGROUND_PTYPE 0x5511
#define MAT_BACKGROUND_BYTES 20
enum { MAT_BG_SUSPEND = 1, MAT_BG_SUSPENDED, MAT_BG_RESUME, MAT_BG_RESUMING, MAT_BG_BLOCKED };
typedef struct {
    uint8_t verb;
    uint32_t serial, generation;
    uint64_t sessionEpoch;
} MAT_BACKGROUND;

static inline void MatBackgroundEncode(uint8_t* out, const MAT_BACKGROUND* m) {
    out[0] = 1; out[1] = m->verb; out[2] = out[3] = 0;
    mat_put32(out + 4, m->serial); mat_put64(out + 8, m->sessionEpoch);
    mat_put32(out + 16, m->generation);
}
static inline bool MatBackgroundDecode(const uint8_t* in, size_t size, MAT_BACKGROUND* m) {
    if (size != MAT_BACKGROUND_BYTES || in[0] != 1 || in[2] || in[3] ||
        in[1] < MAT_BG_SUSPEND || in[1] > MAT_BG_BLOCKED) return false;
    m->verb = in[1]; m->serial = mat_get32(in + 4);
    m->sessionEpoch = mat_get64(in + 8); m->generation = mat_get32(in + 16);
    return m->serial && m->sessionEpoch;
}
