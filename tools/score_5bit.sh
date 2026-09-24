#!/bin/bash
# score.sh <build_dir> -- builds LLM play ROM in that dir, captures opening
# (f400, no input) and gameplay (f800, tapping), scores both vs the
# references in 5-BIT space (guide section 60).  Prints two percentages.
B=$1
cd $B || exit 1
cp /home/claude/pocketvt/builder.py . 2>/dev/null
cp /mnt/user-data/outputs/Lucky_Lawn_Mower_VT09_calibrated.nes . 2>/dev/null
D0=$(arm-none-eabi-nm pocketvt.elf | grep " D _dma0buff$" | cut -d' ' -f1)
cat > sc.c <<CEOF
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char**argv){
  struct mCore*c=GBACoreCreate();c->init(c);mCoreInitConfig(c,NULL);
  mCoreLoadFile(c,argv[1]);
  unsigned w,h;c->desiredVideoDimensions(c,&w,&h);
  uint32_t*v=malloc(w*h*4);c->setVideoBuffer(c,v,w);c->reset(c);
  int N=atoi(argv[2]),mode=atoi(argv[3]);
  for(int f=0;f<N;f++){int k=0;if(mode&&(f%90)<6)k=(f/90)%2?0x08:0x01;c->setKeys(c,k);c->runFrame(c);}
  FILE*f=fopen(argv[4],"wb");fwrite(v,4,38400,f);fclose(f);
  unsigned dp=c->busRead32(c,0x$D0);
  f=fopen(argv[5],"w");
  for(int y=0;y<160;y++) fprintf(f,"%d\n",y+(int)(short)(c->busRead32(c,dp+y*4)>>16));
  fclose(f); return 0;
}
CEOF
gcc -O2 sc.c -o sc -lmgba 2>/dev/null
rm -f *.sav; python3 builder.py Lucky_Lawn_Mower_VT09_calibrated.nes >/dev/null 2>&1
./sc play_me.gba 400 0 op.raw op.geom >/dev/null 2>&1
./sc play_me.gba 800 1 gp.raw gp.geom >/dev/null 2>&1
python3 - <<'PY'
import struct
from PIL import Image
def score(raw,geom,ref):
    fb=open(raw,'rb').read(); px=struct.unpack('<%dI'%(len(fb)//4),fb)
    rows=[int(l) for l in open(geom)]
    R=Image.open(ref).convert('RGB').load()
    q=lambda c:(c[0]>>3,c[1]>>3,c[2]>>3)
    ok=tot=0
    for y in range(160):
        nr=rows[y]
        if not(0<=nr<240): continue
        for x in range(240):
            nx=x+8
            if nx>=256: continue
            p=px[y*240+x]; tot+=1
            ok+= q((p&255,(p>>8)&255,(p>>16)&255))==q(R[nx,nr])
    return 100*ok/tot
print("  opening %5.1f%%   gameplay %5.1f%%"%(score('op.raw','op.geom','/tmp/gg.png'),score('gp.raw','gp.geom','/tmp/lawn.png')))
PY
