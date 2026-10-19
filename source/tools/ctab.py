import sys,struct
for f in sys.argv[1:]:
    b=open(f,'rb').read()
    i=b.find(b'CTAB')
    if i<0: print(f,'none'); continue
    base=i+4  # CTAB header starts after tag
    sz,creator,ver,nconst,infooff=struct.unpack_from('<5I',b,base)
    names=[]
    for k in range(nconst):
        e=base+infooff+k*20
        no,rs,reg,cnt=struct.unpack_from('<IHHH',b,e)
        n=b[base+no:b.index(b'\0',base+no)].decode()
        names.append(f"{n}@c{reg}x{cnt}")
    print(f.split('/')[-1],' '.join(names))
