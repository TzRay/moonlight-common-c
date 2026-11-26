// Compile this client with the original three-field ABI, independent of Limelight.h.
#include <stdint.h>
typedef struct {
    int (*cycle)(void*, const unsigned char*, int, uint64_t, uint64_t, uint64_t, uint64_t);
    int (*end)(void*, uint64_t);
    void* context;
} LEGACY_MAT_CALLBACKS;
extern void LiSetMatPlaybackCallbacks(const LEGACY_MAT_CALLBACKS*);
void registerLegacyMatClient(void* context) {
    const LEGACY_MAT_CALLBACKS callbacks = {0, 0, context};
    LiSetMatPlaybackCallbacks(&callbacks);
}
