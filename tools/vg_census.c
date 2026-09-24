#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <stdio.h>
#include <stdlib.h>
#define KA 0x01
#define KD 0x80
#define VTREG 0x02005888
/* census3 <rom> <cat> <game> [raw]
   Waits for the screen to STOP CHANGING after every input instead of using a
   fixed delay -- the VG menus take ~45 frames to finish drawing and a short
   settle samples mid-redraw (that was the old "G0/G1 never launch" bug). */
static unsigned nth(struct mCore*c){unsigned h=0;for(int i=0;i<960;i++)h=h*31u+c->busRead8(c,0x0203e18c+i);return h;}
static unsigned settle(struct mCore*c,int need,int cap){
  unsigned prev=nth(c); int run=0;
  for(int f=0;f<cap;f++){
    c->setKeys(c,0); c->runFrame(c);
    unsigned x=nth(c);
    run = (x==prev) ? run+1 : 0;
    prev=x;
    if(run>=need) break;
  }
  return prev;
}
static unsigned press(struct mCore*c,int k){
  for(int i=0;i<8;i++){c->setKeys(c,k);c->runFrame(c);}
  return settle(c,20,240);
}
int main(int argc,char**argv){
  struct mCore*c=GBACoreCreate();c->init(c);mCoreInitConfig(c,NULL);
  mCoreLoadFile(c,argv[1]);
  unsigned w,h;c->desiredVideoDimensions(c,&w,&h);
  uint32_t*v=malloc(w*h*4);c->setVideoBuffer(c,v,w);c->reset(c);
  int cat=atoi(argv[2]), game=atoi(argv[3]);
  for(int f=0;f<200;f++){c->setKeys(c,0);c->runFrame(c);}
  settle(c,20,240);
  unsigned ch=press(c,KA);                       /* -> category menu */
  for(int i=0;i<cat;i++) ch=press(c,KD);
  unsigned lh=press(c,KA);                       /* -> game list */
  unsigned gh=lh;
  for(int i=0;i<game;i++) gh=press(c,KD);
  unsigned m[5]; for(int i=0;i<5;i++) m[i]=c->busRead8(c,VTREG+0x07+i);
  for(int i=0;i<8;i++){c->setKeys(c,KA);c->runFrame(c);}
  for(int f=0;f<420;f++){c->setKeys(c,0);c->runFrame(c);}
  unsigned b[5]; for(int i=0;i<5;i++) b[i]=c->busRead8(c,VTREG+0x07+i);
  int same=1; for(int i=0;i<5;i++) if(b[i]!=m[i]) same=0;
  uint32_t bg=v[0];int lit=0,ns=0;uint32_t s[64];
  for(int i=0;i<38400;i++){if(v[i]!=bg)lit++;int f=0;for(int j=0;j<ns;j++)if(s[j]==v[i]){f=1;break;}if(!f&&ns<64)s[ns++]=v[i];}
  const char*verdict = same ? "NOLAUNCH" : (lit==0? "BLANK   " : (ns<=2? "FLAT    " : (lit<4000? "SPARSE  " : "OK      ")));
  printf("C%d G%-2d %s lit=%-6d col=%-3d $2010=%02x bank=%02x:%02x:%02x:%02x:%02x sel#%08x\n",
    cat,game,verdict,lit,ns,c->busRead8(c,0x02000b8c),b[0],b[1],b[2],b[3],b[4],gh);
  if(argc>4){FILE*f=fopen(argv[4],"wb");fwrite(v,4,38400,f);fclose(f);}
  return 0;
}
