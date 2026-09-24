#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <stdio.h>
#include <stdlib.h>
static void hold(struct mCore*c,int k,int n){for(int i=0;i<n;i++){c->setKeys(c,k);c->runFrame(c);}}
/* vgfail <rom> <downs>  -- category 0, entry <downs> */
int main(int argc,char**argv){
  struct mCore*c=GBACoreCreate();c->init(c);mCoreInitConfig(c,NULL);
  mCoreLoadFile(c,argv[1]);
  unsigned w,h;c->desiredVideoDimensions(c,&w,&h);
  uint32_t*v=malloc(w*h*4);c->setVideoBuffer(c,v,w);c->reset(c);
  int downs=atoi(argv[2]);
  hold(c,0,200);                       /* title            */
  hold(c,1,8); hold(c,0,40);           /* A -> category    */
  hold(c,1,8); hold(c,0,40);           /* A -> game list   */
  for(int i=0;i<downs;i++){ hold(c,4,8); hold(c,0,20); }   /* Down */
  hold(c,1,8); hold(c,0,420);          /* A -> launch      */
  uint32_t bg=v[0];int lit=0,ns=0;uint32_t s[64];
  for(int i=0;i<38400;i++){if(v[i]!=bg)lit++;int f=0;for(int j=0;j<ns;j++)if(s[j]==v[i]){f=1;break;}if(!f&&ns<64)s[ns++]=v[i];}
  printf("entry %d: lit=%d colours=%d $2010=%02x\n",downs,lit,ns,c->busRead8(c,0x02000b8c));
  printf("  $2012-17 =");for(int i=0;i<6;i++)printf(" %02x",c->busRead8(c,0x02005cfd+i));
  printf("\n  $2018=%02x $201A=%02x $4100outer=%02x\n",
    c->busRead8(c,0x02005d03),c->busRead8(c,0x02005d04),c->busRead8(c,0x02005d05));
  return 0;
}
