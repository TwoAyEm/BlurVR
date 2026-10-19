import struct,sys
OPS={0:'nop',1:'mov',2:'add',3:'sub',4:'mad',5:'mul',6:'rcp',7:'rsq',8:'dp3',9:'dp4',10:'min',11:'max',12:'slt',13:'sge',14:'exp',15:'log',16:'lit',17:'dst',18:'lrp',19:'frc',20:'m4x4',21:'m4x3',22:'m3x4',23:'m3x3',24:'m3x2',25:'call',26:'callnz',27:'loop',28:'ret',29:'endloop',30:'label',31:'dcl',32:'pow',33:'crs',34:'sgn',35:'abs',36:'nrm',37:'sincos',38:'rep',39:'endrep',40:'if',41:'ifc',42:'else',43:'endif',44:'break',45:'breakc',46:'mova',65:'texkill',66:'texld',78:'cmp',79:'bem',80:'dp2add',81:'def',82:'defb',83:'defi',88:'dp2add',89:'dsx',90:'dsy',91:'texldd',92:'setp',93:'texldl',94:'breakp'}
RT={0:'r',1:'v',2:'c',3:'t',4:'oPos',5:'oD',6:'oT',7:'i',8:'oC',9:'oDepth',10:'s',11:'c',12:'c',13:'c',14:'b',15:'aL',16:'r',17:'vPos',18:'l',19:'p'}
def regtype(t): return ((t>>28)&7)|(((t>>11)&3)<<3)
def regname(t,pshader3=True):
    ty=regtype(t); n=t&0x7ff
    nm=RT.get(ty,'?%d'%ty)
    if ty==3 and not pshader3: nm='t'
    if ty==1 and True: nm='v'
    return '%s%d'%(nm,n)
def swz(t):
    s=(t>>16)&0xff; return ''.join('xyzw'[(s>>(2*i))&3] for i in range(4))
def mask(t):
    m=(t>>16)&0xf; return ''.join(c for i,c in enumerate('xyzw') if m&(1<<i))
MOD={0:'',1:'-',2:'bias',3:'-bias',4:'sign',5:'-sign',6:'comp',7:'x2',8:'-x2',9:'dz',10:'dw',11:'abs',12:'-abs',13:'!'}
def dis(path):
    d=open(path,'rb').read(); w=struct.unpack('<%dI'%(len(d)//4),d)
    print('version %08x'%w[0]); i=1; out=[]
    while i<len(w):
        t=w[i]
        if t==0xffff: break
        if (t&0xffff)==0xfffe: i+=1+((t>>16)&0x7fff); continue
        op=t&0xffff; ln=(t>>24)&0xf; args=w[i+1:i+1+ln]; name=OPS.get(op,'op%d'%op); i+=1+ln
        if op==31:
            u=args[0]; dst=args[1]; print('dcl_%s %s'%(['position','blendweight','blendindices','normal','psize','texcoord','tangent','binormal','tessfactor','positiont','color','fog','depth','sample'][u&0x1f] if regtype(dst)!=10 else ['','','2d','cube','volume'][(u>>27)&0xf], regname(dst)+'.'+mask(dst)));continue
        if op==81:
            print('def %s, %g, %g, %g, %g'%(regname(args[0]),*struct.unpack('<4f',struct.pack('<4I',*args[1:5])))); continue
        if op in(82,83): print(name,args);continue
        # dst first for most
        if op in (28,29,39,42,43,44,0): print(name); continue
        s=name; 
        if op in (40,41,38,27,25,26,30,45,94,65):
            s+=' '+', '.join(regname(a)+'.'+swz(a) for a in args); print(s); continue
        dst=args[0]; sat=''
        if (dst>>20)&1: sat='_sat'
        if (dst>>20)&2: sat+='_pp'
        parts=[]
        for a in args[1:]:
            m=MOD.get((a>>24)&0xf,'m?'); r=regname(a)+('[rel]' if (a>>13)&1 else '')+'.'+swz(a)
            if m=='-': r='-'+r
            elif m=='abs': r='abs(%s)'%r
            elif m: r='%s(%s)'%(m,r)
            parts.append(r)
        print('%s%s %s.%s, %s'%(name,sat,regname(dst),mask(dst),', '.join(parts)))
dis(sys.argv[1])
