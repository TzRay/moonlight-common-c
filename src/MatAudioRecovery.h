#pragma once
#include "MatWire.h"

/* MAT-only messages on the authenticated reliable control channel. v1 keeps
 * its original 56-byte wire layout. v2 adds SSRC and repeatable attempts. */
#define MAT_RECOVERY_PACKET_TYPE 0x550F
#define MAT_RECOVERY_BYTES 56
#define MAT_RECOVERY_V2_BYTES 60
#define MAT_RECOVERY_REQUEST 1
#define MAT_RECOVERY_PREPARE 2
#define MAT_RECOVERY_READY 3
#define MAT_RECOVERY_START 4
#define MAT_RECOVERY_FAIL 5
#define MAT_RECOVERY_PLAYING 6
#define MAT_RECOVERY_STABLE 7
#define MAT_RECOVERY_RETRYABLE 1
#define MAT_RECOVERY_BLOCKED 2
#define MAT_RECOVERY_TIMEOUT_MS 10000
#define MAT_RECOVERY_STABLE_MS 30000

typedef struct {
    uint8_t verb;
    uint32_t generation;
    uint64_t previousEpoch, newEpoch, domain;
    uint32_t reason;
    uint16_t serverPort, clientPort;
    uint8_t token[16];
    uint8_t version; /* zero-initialized legacy callers mean v1 */
    uint32_t ssrc;
} MAT_AUDIO_RECOVERY;

static inline unsigned MatRecoveryVersion(const MAT_AUDIO_RECOVERY* m) { return m->version ? m->version : 1; }
static inline size_t MatRecoverySize(const MAT_AUDIO_RECOVERY* m) {
    return MatRecoveryVersion(m) == 2 ? MAT_RECOVERY_V2_BYTES : MAT_RECOVERY_BYTES;
}
static inline void MatRecoveryEncode(uint8_t* out, const MAT_AUDIO_RECOVERY* m) {
    memset(out, 0, MatRecoverySize(m));
    out[0]=(uint8_t)MatRecoveryVersion(m);out[1]=m->verb;
    mat_put32(out+4,m->generation);mat_put64(out+8,m->previousEpoch);
    mat_put64(out+16,m->newEpoch);mat_put64(out+24,m->domain);mat_put32(out+32,m->reason);
    out[36]=(uint8_t)m->serverPort;out[37]=(uint8_t)(m->serverPort>>8);
    out[38]=(uint8_t)m->clientPort;out[39]=(uint8_t)(m->clientPort>>8);
    memcpy(out+40,m->token,16);
    if(out[0]==2)mat_put32(out+56,m->ssrc);
}
static inline int MatRecoveryDecode(const uint8_t* in, size_t size, MAT_AUDIO_RECOVERY* m) {
    if(!in||!m||size<MAT_RECOVERY_BYTES||in[2]||in[3]||
       (in[0]!=1&&in[0]!=2)||size!=(in[0]==2?MAT_RECOVERY_V2_BYTES:MAT_RECOVERY_BYTES)||
       in[1]<MAT_RECOVERY_REQUEST||in[1]>(in[0]==2?MAT_RECOVERY_STABLE:MAT_RECOVERY_FAIL))return 0;
    memset(m,0,sizeof(*m));m->version=in[0];m->verb=in[1];m->generation=mat_get32(in+4);
    m->previousEpoch=mat_get64(in+8);m->newEpoch=mat_get64(in+16);m->domain=mat_get64(in+24);
    m->reason=mat_get32(in+32);m->serverPort=(uint16_t)(in[36]|((uint16_t)in[37]<<8));
    m->clientPort=(uint16_t)(in[38]|((uint16_t)in[39]<<8));memcpy(m->token,in+40,16);
    if(in[0]==2)m->ssrc=mat_get32(in+56);
    if(!m->previousEpoch||m->previousEpoch>INT64_MAX||
       (in[0]==1&&m->generation!=1))return 0;
    unsigned present=0;for(unsigned i=0;i<16;i++)present|=m->token[i];
    if(m->verb==MAT_RECOVERY_REQUEST)return !m->newEpoch&&!m->domain&&!m->serverPort&&!m->clientPort&&!present&&!m->ssrc&&
        (in[0]==1||m->reason==MAT_RECOVERY_RETRYABLE||m->reason==MAT_RECOVERY_BLOCKED);
    if(in[0]==1&&m->verb==MAT_RECOVERY_FAIL)return 1;
    if(in[0]==2&&(!m->generation||!m->ssrc||
       (m->reason!=MAT_RECOVERY_RETRYABLE&&m->reason!=MAT_RECOVERY_BLOCKED)))return 0;
    return m->newEpoch&&m->newEpoch<=INT64_MAX&&m->newEpoch!=m->previousEpoch&&
        m->domain&&m->domain<=INT64_MAX&&m->serverPort&&present&&
        (m->verb==MAT_RECOVERY_PREPARE?!m->clientPort:
         m->verb==MAT_RECOVERY_FAIL||m->clientPort!=0);
}
static inline int MatRecoveryMatchesProfile(const MAT_AUDIO_RECOVERY* a, const MAT_AUDIO_RECOVERY* b) {
    return MatRecoveryVersion(a)==MatRecoveryVersion(b)&&a->generation==b->generation&&
        a->previousEpoch==b->previousEpoch&&a->newEpoch==b->newEpoch&&a->domain==b->domain&&
        a->ssrc==b->ssrc&&a->serverPort==b->serverPort&&!memcmp(a->token,b->token,16);
}

/* Only the host schedules retries. Healthy playback, not START, resets this. */
static inline unsigned MatRecoveryCooldownMs(unsigned attempts) {
    if(!attempts)return 0;
    return attempts>=5?5000:250u<<(attempts-1);
}
