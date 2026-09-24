#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <stdio.h>
#include <stdlib.h>
#define M6502PC  0x030076b4
static void hold(struct mCore*c,int k,int n){for(int i=0;i<n;i++){c->setKeys(c,k);c->runFrame(c);}}
int main(int argc,char**argv){
  struct mCore*c=GBACoreCreate();c->init(c);mCoreInitConfig(c,NULL);
  mCoreLoadFile(c,argv[1]);
  unsigned w,h;c->desiredVideoDimensions(c,&w,&h);
  uint32_t*v=malloc(w*h*4);c->setVideoBuffer(c,v,w);c->reset(c);
  unsigned LB=strtoul(argv[3],0,16);
  int downs=atoi(argv[2]);
  hold(c,0,200); hold(c,1,8); hold(c,0,40); hold(c,1,8); hold(c,0,40);
  for(int i=0;i<downs;i++){ hold(c,4,8); hold(c,0,20); }
  hold(c,1,8); hold(c,0,300);
  printf("entry %s guest PC samples (pc - lastbank):\n  ",argv[2]);
  for(int i=0;i<12;i++){
    unsigned pc=c->busRead32(c,M6502PC), lb=c->busRead32(c,LB);
    printf("%04X ",(pc-lb)&0xFFFF);
    c->setKeys(c,0); c->runFrame(c);
  }
  printf("\n");
  return 0;
}
