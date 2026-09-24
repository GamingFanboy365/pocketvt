import struct, sys
from PIL import Image
def score(fb,geom,refpath):
    d=struct.unpack('<%dI'%(240*160),open(fb,'rb').read()[:240*160*4])
    O=[((x&0xFF)>>3,((x>>8)&0xFF)>>3,((x>>16)&0xFF)>>3) for x in d]
    G=[tuple(map(int,l.split())) for l in open(geom)]
    im=Image.open(refpath).convert('RGB'); W,H=im.size
    rp=[(r>>3,g>>3,b>>3) for r,g,b in list(im.getdata())]
    S=H/240.0
    ex=st=tot=0
    for y in range(159):
        ny=int((y+G[y][1])*S)
        if not (0<=ny<H): continue
        for x in range(239):
            nx=x+8
            if nx>=W-1: continue
            tot+=1
            if O[y*240+x]==rp[ny*W+nx]: ex+=1
            if (O[y*240+x]==O[y*240+x+1])==(rp[ny*W+nx]==rp[ny*W+nx+1]): st+=1
    return 100.0*ex/tot,100.0*st/tot,S
if __name__=="__main__":
    e,s,S=score(*sys.argv[1:4])
    print("   %-12s exact %5.1f%%  structural %5.1f%%   (vertical scale %.3f)"%(sys.argv[4],e,s,S))
