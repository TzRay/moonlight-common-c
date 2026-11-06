/* The real client RTP/FEC queue and AudioStream decode path. Synthetic carrier only. */
#include "../src/AudioStream.c"
#include "MatLiveCrypto.h"
#include <stdio.h>
static int failures,cycles,ends,terminated;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);++failures;}}while(0)
static int cycle(void* c,const uint8_t* bytes,int n,uint64_t e,uint64_t d,uint64_t index,uint64_t pts){(void)c;(void)bytes;CHECK(n==61440 && e==123 && d==7 && index==(unsigned)cycles && pts==index*960);++cycles;return 1;}
static int end(void* c,uint64_t n){(void)c;CHECK(n==(unsigned)cycles);++ends;return 1;}
static void terminate(int error){(void)error;++terminated;}
static void packet(uint64_t sequence,const uint8_t* payload,size_t size){
 PQUEUED_AUDIO_PACKET p=calloc(1,sizeof(*p));if(!p){++failures;return;}
 p->header.size=sizeof(RTP_PACKET)+(int)size;PRTP_PACKET r=(PRTP_PACKET)p->data;
 r->header=0x80;r->packetType=97;r->sequenceNumber=(uint16_t)sequence;r->timestamp=(uint32_t)sequence;
 memcpy(r+1,payload,size);int status=RtpaAddPacket(&rtpAudioQueue,r,(uint16_t)p->header.size);
 if(RTPQ_HANDLE_NOW(status))decodeInputData(p);
 else {
  uint16_t length;PQUEUED_AUDIO_PACKET queued;
  while((queued=(PQUEUED_AUDIO_PACKET)RtpaGetQueuedPacket(&rtpAudioQueue,sizeof(QUEUED_AUDIO_PACKET_HEADER),&length))!=NULL){queued->header.size=length;decodeInputData(queued);free(queued);}
 }
 free(p);
}
static void missingBlockDeadline(void){
 RTP_AUDIO_QUEUE q;RtpaInitializeQueue(&q);
 uint8_t data[sizeof(RTP_PACKET)+MAT_LIVE_PACKET]={0};PRTP_PACKET r=(PRTP_PACKET)data;
 r->header=0x80;r->packetType=97;
 for(unsigned sequence=0;sequence<8;sequence++){
  r->sequenceNumber=(uint16_t)sequence;r->timestamp=sequence;
  RtpaAddPacket(&q,r,sizeof(data));
 }
 r->sequenceNumber=16;r->timestamp=16;
 CHECK(!RTPQ_HANDLE_NOW(RtpaAddPacket(&q,r,sizeof(data))));
 uint16_t length;PRTP_PACKET output=RtpaGetQueuedPacket(&q,0,&length);
 CHECK(output==NULL);free(output);
 PltSleepMs(RTPQ_OOS_WAIT_TIME_MS+5);
 r->sequenceNumber=17;r->timestamp=17;RtpaAddPacket(&q,r,sizeof(data));
 output=RtpaGetQueuedPacket(&q,0,&length);
 CHECK(output && output->sequenceNumber==16);free(output);
 RtpaCleanupQueue(&q);
}
int main(void){
 CHECK(initializePlatform()==0);AppVersionQuad[0]=7;AppVersionQuad[1]=1;AppVersionQuad[2]=431;AppVersionQuad[3]=-1;
 NegotiatedAudioCodec=AUDIO_CODEC_MAT_LIVE;AudioEncryptionEnabled=true;AudioPacketDuration=1;ListenerCallbacks.connectionTerminated=terminate;
 missingBlockDeadline();RtpaInitializeQueue(&rtpAudioQueue);lastSeq=3;
 LI_MAT_LIVE_PROFILE profile={123,7};uint8_t key[16]={};LI_MAT_PLAYBACK_CALLBACKS cb={0};cb.cycle=cycle;cb.end=end;
 matLive=MatLivePlaybackOpen(&profile,key,&cb);CHECK(matLive);
 MAT_LIVE_CIPHER cipher;CHECK(MatLiveCipherInit(&cipher,key,16,123,7));
 uint8_t plain[MAT_WIRE_PACKET],wire[MAT_LIVE_PACKET],carrier[MAT_WIRE_CYCLE]={0};
 memcpy(carrier,"\x72\xf8\x1f\x4e\x16\x00\x02\x00",8);
 mat_wire_make(plain,MAT_WIRE_SYNC,123,7,0,0,0,NULL);uint64_t next=0;
 for(unsigned i=0;i<4;i++){CHECK(MatLiveSeal(&cipher,next,plain,wire));packet(next++,wire,sizeof(wire));}
 uint32_t hash=2166136261u;uint8_t delayed[MAT_LIVE_PACKET],delayedBlocks[8][MAT_LIVE_PACKET];
 for(unsigned i=0;i<1300;i++){
  carrier[8]=(uint8_t)i;uint32_t h=mat_hash(carrier,sizeof(carrier));
  for(unsigned j=0;j<60;j++){
   mat_wire_make(plain,MAT_WIRE_DATA,123,7,i,j,h,carrier);
   uint64_t sequence=next++;CHECK(MatLiveSeal(&cipher,sequence,plain,wire));
   // The last shard of the first data block arrives after the next block starts.
   if(i==0&&j==3){memcpy(delayed,wire,sizeof(delayed));continue;}
   // Two complete blocks can arrive behind the next one, including across RTP16 wrap.
   if((i==1||i==1092)&&j>=8&&j<16){memcpy(delayedBlocks[j-8],wire,sizeof(wire));continue;}
   packet(sequence,wire,sizeof(wire));
   if(i==0&&j==4)packet(sequence-1,delayed,sizeof(delayed));
   if((i==1||i==1092)&&j==16){
    for(unsigned k=0;k<8;k++)packet(sequence-8+k,delayedBlocks[k],sizeof(delayedBlocks[k]));
    CHECK(!terminated);if(terminated)goto cleanup;
   }
  }
  hash=mat_hash_update(hash,carrier,sizeof(carrier));
 }
 mat_wire_make(plain,MAT_WIRE_END,123,7,1300,0,hash,NULL);
 for(unsigned i=0;i<4;i++){CHECK(MatLiveSeal(&cipher,next,plain,wire));packet(next++,wire,sizeof(wire));}
 CHECK(cycles==1300 && ends==1 && !terminated && lastSeq==(uint16_t)(next-1));
cleanup:
 MatLivePlaybackClose(matLive);matLive=NULL;RtpaCleanupQueue(&rtpAudioQueue);cleanupPlatform();
 if(failures)return 1;puts("PASS actual RTP queue / AudioStream: 78008 packets, partial/whole-block reordering, bounded missing-block wait, RTP16 wrap, legacy CBC bypass, exact authenticated cycles and EOS");return 0;
}
