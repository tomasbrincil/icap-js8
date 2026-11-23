#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
#
# rv_protocol.py - adversarial protocol-fidelity check of the JS8 receiver (review of 23 Nov 2026).
# A SECOND, independent model written straight from the JS8Call sources that are kept as data in
# $JS8_REF/js8call (tools/fetch_refs.sh; genjs8.f90, encode174.f90, ldpc_174_87_params.f90,
# bpdecode174.f90, crc12.cpp, varicode.cpp, decodedtext.cpp, jsc.cpp, jsc_map.cpp; the copies are
# byte-identical to js8call/js8call master, checked with curl on 23 Nov 2026).  It shares no code
# with tests/model.py or src/js8pack.c.  Nothing is compiled from JS8Call.
#
#   python3 -I -B tests/rv_protocol.py [--n 300000] [--seed 1]
# builds tests/rv_dump.c into $BUILD/rv/ and compares the product code with the model:
#   1 encoder: frame bits, CRC-12 (both product implementations), LDPC parity, tones, Costas
#   2 LDPC Nm / Mn tables against bpdecode174.f90
#   3 dictionary files on the SD card against JSC::map (all 262144 words)
#   4 frame -> text: random and edge-case frames of every type against DecodedText::message()
#   5 reassembly / log scenarios (what JS8Call would show is stated per scenario)
# Exit 1 if a check marked FAIL fails.  Divergences found by the review are printed as "KNOWN"
# with their count (they still make the run exit 1 - the test is not weakened to pass).
import os, re, sys, random, subprocess, collections
_ROOT=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC=os.path.join((os.environ.get('JS8_REF') or _ROOT + '/third_party'),'js8call')
BUILD=os.environ.get('BUILD') or _ROOT+'/build'
SD=os.path.join(BUILD,'sd')
HERE=os.path.dirname(os.path.abspath(__file__))
JS8=os.path.dirname(HERE)
DUMP=os.path.join(BUILD,'rv','rv_dump')

# ---- JSC::map parser (C string literals) -----------------------------------------------------
def cstr(d, i):
    # d[i] == '"'; returns (bytes, next index); handles adjacent-literal concatenation not needed
    out = bytearray(); i += 1
    while d[i] != 0x22:
        c = d[i]
        if c == 0x5c:
            n = d[i+1]
            if n in b'xX':
                j = i+2; v = 0
                while chr(d[j]) in '0123456789abcdefABCDEF':
                    v = v*16 + int(chr(d[j]),16); j += 1
                out.append(v & 255); i = j
            elif chr(n) in '01234567':
                j = i+1; v = 0; k = 0
                while k < 3 and chr(d[j]) in '01234567':
                    v = v*8+int(chr(d[j])); j += 1; k += 1
                out.append(v & 255); i = j
            else:
                m = {ord('n'):10, ord('t'):9, ord('r'):13, ord('0'):0, ord('a'):7}
                out.append(m.get(n, n)); i += 2
        else:
            out.append(c); i += 1
    return bytes(out), i+1
def parse(path, name):
    d = open(path,'rb').read()
    s = d.index(b'JSC::'+name+b'[')
    s = d.index(b'{', s)+1
    rows = []
    i = s
    while True:
        while d[i] in b' \t\r\n,': i += 1
        if d[i:i+2] == b'//':
            i = d.index(b'\n', i); continue
        if d[i] == ord('}'): break
        assert d[i] == ord('{'), (i, d[i:i+40])
        i += 1
        while d[i] in b' \t': i += 1
        st, i = cstr(d, i)
        # possible adjacent literal
        while True:
            while d[i] in b' \t': i += 1
            if d[i:i+2] == b'/*':
                i = d.index(b'*/', i)+2
            elif d[i] == 0x22:
                s2, i = cstr(d, i); st += s2
            else: break
        e = d.index(b'}', i)
        a, b = [int(x) for x in d[i:e].replace(b',', b' ').split()]
        rows.append((st, a, b)); i = e+1
    return rows

# ---- genjs8 / encode174 / crc12 model ---------------------------------------------------------
S=os.path.join(SRC,'lib/ft8/')
p=open(S+'ldpc_174_87_params.f90').read()
g=re.findall(r'"([0-9a-f]{22})"',p)
assert len(g)==87
colorder=[int(x) for x in re.search(r'data colorder/(.*?)/',p,re.S).group(1).replace('&','').replace('\n','').split(',')]
assert len(colorder)==174 and sorted(colorder)==list(range(174))
bp=open(S+'bpdecode174.f90').read()
def data(name):
    s=re.search(r'data '+name+r'/(.*?)/',bp,re.S).group(1)
    return [int(x) for x in re.findall(r'-?\d+',s)]
assert data('colorder')==colorder
Mn=data('Mn'); Nm=data('Nm'); nrw=data('nrw')
assert len(Mn)==3*174 and len(Nm)==7*87 and len(nrw)==87
MnT=[Mn[3*i:3*i+3] for i in range(174)]   # Mn(3,N) column major: Mn(:,j)
NmT=[Nm[7*i:7*i+7] for i in range(87)]
gen=[[ (int(r,16)>>(87-j))&1 for j in range(87)] for r in g]  # 88 bits hex, first 87 used
def aug_crc(bits):   # boost::augmented_crc<12,0xc06>, init 0: shift message bits in
    rem=0
    for b in bits:
        top=rem&0x800
        rem=((rem<<1)|b)&0xfff
        if top: rem^=0xc06
    return rem
ALPH='0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+/?.'
def genjs8(msg,i3):
    w=[ALPH.index(c) for c in msg]
    bits=[]
    for v in w: bits+=[(v>>(5-k))&1 for k in range(6)]
    bits+=[(i3>>2)&1,(i3>>1)&1,i3&1]
    by=bits+[0]*13     # 11 bytes: byte 10 masked to top 3 bits, byte 11 zero
    crc=aug_crc(by)^42
    msgbits=bits+[(crc>>(11-k))&1 for k in range(12)]
    pch=[sum(a*b for a,b in zip(msgbits,gen[i]))%2 for i in range(87)]
    itmp=pch+msgbits
    cw=[0]*174
    for i in range(174): cw[colorder[i]]=itmp[i]
    # parity check with Nm from the decoder source (1-origin, transmitted order)
    for j in range(87):
        assert sum(cw[NmT[j][i]-1] for i in range(nrw[j]))%2==0
    tone=[4,2,5,6,1,3,0]+[None]*29+[4,2,5,6,1,3,0]+[None]*29+[4,2,5,6,1,3,0]
    k=7-1
    for j in range(1,59):
        k+=1
        if j==30: k+=7
        i=3*j-3
        tone[k]=cw[i]*4+cw[i+1]*2+cw[i+2]
    return msgbits,crc,tone

# ---- Varicode / DecodedText / JSC receive model ----------------------------------------------
A72="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+/?."
AN="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@"
CMDS={" HEARTBEAT":-1," HB":-1," CQ":-1," SNR?":0,"?":0," DIT DIT":1," HEARING?":3," GRID?":4,">":5," STATUS?":6," STATUS":7," HEARING":8," MSG":9," MSG TO:":10," QUERY":11," QUERY MSGS":12," QUERY MSGS?":12," QUERY CALL":13," GRID":15," INFO?":16," INFO":17," FB":18," HW CPY?":19," SK":20," RR":21," QSL?":22," QSL":23," CMD":24," SNR":25," NO":26," YES":27," 73":28," NACK":2," ACK":14," HEARTBEAT SNR":29," AGN?":30,"  ":31," ":31}
def cmdkey(v):   # QMap::key(value): first key in sorted key order, default ""
    for k in sorted(CMDS):
        if CMDS[k]==v: return k
    return ""
SNR={25,29}
def is_snr(c): return c in CMDS and CMDS[c] in SNR
HUFF={" ":"01","E":"100","T":"1101","A":"0011","O":"11111","I":"11100","N":"10111","S":"10100","H":"00011","R":"00000","D":"111011","L":"110011","C":"110001","U":"101101","M":"101011","W":"001011","F":"001001","G":"000101","Y":"000011","P":"1111011","B":"1111001",".":"1110100","V":"1100101","K":"1100100","-":"1100001","+":"1100000","?":"1011001","!":"1011000","\"":"1010101","X":"1010100","0":"0010101","J":"0010100","1":"0010001","Q":"0010000","2":"0001001","Z":"0001000","3":"0000101","5":"0000100","4":"11110101","9":"11110100","8":"11110001","6":"11110000","7":"11101011","/":"11101010"}
NB=37*36*10*27*27*27; NG=180*180; NU=NG+10; NM=(1<<15)-1
GROUPS="<....> @ALLCALL @JS8NET @DX/NA @DX/SA @DX/EU @DX/AS @DX/AF @DX/OC @DX/AN @REGION/1 @REGION/2 @REGION/3 @GROUP/0 @GROUP/1 @GROUP/2 @GROUP/3 @GROUP/4 @GROUP/5 @GROUP/6 @GROUP/7 @GROUP/8 @GROUP/9 @COMMAND @CONTROL @NET @NTS @RESERVE/0 @RESERVE/1 @RESERVE/2 @RESERVE/3 @RESERVE/4 @APRSIS @RAGCHEW @JS8 @EMCOMM @ARES @MARS @AMRRON @RACES @RAYNET @RADAR @SKYWARN @CQ @HB @QSO @QSOPARTY @CONTEST @FIELDDAY @SOTA @IOTA @POTA @QRP @QRO".split()
BASE={NB+1+i:g for i,g in enumerate(GROUPS)}
CQS=["CQ CQ CQ","CQ DX","CQ QRP","CQ CONTEST","CQ FIELD","CQ FD","CQ CQ","CQ"]
MAP=None
def jmap():
    global MAP
    if MAP is None: MAP=parse(os.path.join(SRC,'jsc_map.cpp'), b'map')
    return MAP
def qmid(l,pos,n=-1):   # QVector/QList::mid
    if pos>len(l): return l[:0]
    if n<0: return l[pos:]
    return l[pos:pos+n]
def unpack72(t):
    v=0
    for i in range(10): v|=A72.index(t[i])<<(58-6*i)
    rh=A72.index(t[10]); v|=rh>>2
    rem=((rh&3)<<6)|A72.index(t[11])
    return v,rem
def bits(v,n): return [(v>>(n-1-i))&1 for i in range(n)]
def b2i(b):
    v=0
    for x in b: v=(v<<1)|x
    return v
def formatSNR(s):
    if s<-60 or s>60: return ""
    return ("+" if s>=0 else "")+("%03d"%s if s<0 else "%02d"%s)
def deg2grid(dlong,dlat):
    if dlong<-180: dlong+=360
    if dlong>180: dlong-=360
    nlong=int(60.0*(180.0-dlong)/5)
    n1=nlong//240; n2=(nlong-240*n1)//24
    g=[None]*4
    g[0]=chr(65+n1); g[2]=chr(48+n2)
    nlat=int(60.0*(dlat+90)/2.5)
    n1=nlat//240; n2=(nlat-240*n1)//24
    g[1]=chr(65+n1); g[3]=chr(48+n2)
    return ''.join(g)
def unpackGrid(v):
    if v>NG: return ""
    return deg2grid(float(v//180*2-180+2), float(v%180-90))
def unpackCallsign(v,portable):
    if v in BASE: return BASE[v]
    w=[None]*6
    w[5]=AN[v%27+10]; v//=27
    w[4]=AN[v%27+10]; v//=27
    w[3]=AN[v%27+10]; v//=27
    w[2]=AN[v%10]; v//=10
    w[1]=AN[v%36]; v//=36
    w[0]=AN[v]
    c=''.join(w)
    if c.startswith("3D0"): c="3DA0"+c[3:]
    if c.startswith("Q") and 'A'<=c[1]<='Z': c="3X"+c[1:]
    if portable: c=c.strip()+"/P"
    return c.strip()
def unpackAN50(p):
    w=[None]*11
    for i,(r,sl) in zip(range(10,-1,-1),[(38,0),(38,0),(38,0),(2,1),(38,0),(38,0),(38,0),(2,1),(38,0),(38,0),(39,0)]):
        t=p%r; p//=r
        w[i]=('/' if t else ' ') if sl else AN[t]
    return ''.join(w).replace(' ','')
def unpackCmd(v):
    if v&0x80:
        return (29 if v&0x40 else 25), v&0x3f
    return v&0x7f, 0
def huffDecode(bv):
    s=''.join(map(str,bv)); text=''
    keys=sorted(HUFF)
    while s:
        found=False
        for k in keys:
            if s.startswith(HUFF[k]):
                text+=k; s=s[len(HUFF[k]):]; found=True
        if not found: break
    return text
def jsc_decompress(bv, honour_size=True):
    s=7; c=9; SIZE=262144
    base=[0,s]
    for k in range(2,8): base.append(base[-1]+s*c**(k-1))
    by=[]; seps=[]
    i=0; n=len(bv)
    while i<n:
        b=bv[i:i+4]
        if len(b)!=4: break
        v=b2i(b); by.append(v); i+=4
        if v<s:
            if n-i>0 and bv[i]: seps.append(len(by)-1)
            i+=1
    out=[]; start=0
    while start<len(by):
        k=0; j=0
        while start+k<len(by) and by[start+k]>=s:
            j=(j*c+(by[start+k]-s)) & 0xffffffff; k+=1
        if j>=SIZE: break
        if start+k>=len(by): break
        if k>7: out.append(('UB',)); break     # base[k] out of bounds in JS8Call
        j=(j*s+by[start+k]+base[k]) & 0xffffffff
        if j>=SIZE: break
        st,sz,_=jmap()[j]
        out.append(st[:sz] if honour_size else st)
        if seps and seps[0]==start+k:
            out.append(b' '); seps.pop(0)
        start+=k+1
    if any(o==('UB',) for o in out): return None
    return b''.join(out)
def decode(frame, i3, honour_size=True):
    """returns message() bytes exactly as DecodedText would hold (latin1)"""
    v,rem=unpack72(frame)
    B=bits(v,64)+bits(rem,8)
    isdata=(i3&4)==4
    # 1 fast data
    if isdata:
        n=len(B)-1-B[::-1].index(0) if 0 in B else -1
        r=jsc_decompress(qmid(B,0,n),honour_size)
        if r is None: return None
        if r: return r
        return frame.encode()
    # 2 data
    if B[0]:
        b=B[1:]
        comp=b[0]
        n=len(b)-1-b[::-1].index(0) if 0 in b else -1
        b=qmid(b,1,n-1)
        if comp:
            r=jsc_decompress(b,honour_size)
            if r is None: return None
        else: r=huffDecode(b).encode()
        if r: return r
    # compound frame fields
    b64=bits(v,64)
    flag=b2i(b64[0:3])
    p5=rem>>3; p3=rem&7
    call=unpackAN50(b2i(b64[3:53])); num=(b2i(b64[53:64])<<5)|p5
    if flag==0:   # heartbeat
        grid=unpackGrid(num&0x7fff)
        m=call+": "
        if num&0x8000: m+="@ALLCALL "+CQS[p3]
        else: m+="@HB HEARTBEAT"
        return (m+' '+grid+' ').encode()
    if flag in (1,2):
        parts=[call,""]
        if num<=NG: parts.append(" "+unpackGrid(num))
        elif NU<=num<NM:
            cmd,n6=unpackCmd((num-NU)&0xff)
            cs=cmdkey(cmd); parts.append(cs)
            if is_snr(cs): parts.append(formatSNR(n6-31))
        extra=' '.join(parts[2:])
        if flag==1: return (call+": ").encode()
        return (call+extra+' ').encode()
    if flag==3:
        pf=b2i(b64[3:31]); pt=b2i(b64[31:59]); pc=b2i(b64[59:64])
        ex=rem
        fr=unpackCallsign(pf,(ex>>7)&1); to=unpackCallsign(pt,(ex>>6)&1)
        ex%=64
        cmd=cmdkey(pc%32)
        parts=[fr,to,cmd]
        if ex!=0:
            parts.append(formatSNR(ex-31) if is_snr(cmd) else "%d"%(ex-31))
        return (parts[0]+": "+parts[1]+' '.join(parts[2:])+' ').encode()
    return frame.encode()

# ---- frame builders for the scenarios (Varicode pack*, JSC::codeword) -------------------------
def ib(v,n): return bits(v,n)
def frame(bits72,i3):
    b=list(bits72)+ib(i3,3)
    crc=aug_crc(b+[0]*13)^42
    b+=ib(crc,12)+[0]
    return '%022x'%b2i(b)
def call28(c):
    if c in GROUPS: return NB+1+GROUPS.index(c)
    w=c if c[2].isdigit() and len(c)<=6 else ' '+c
    w=w.ljust(6)
    assert w[2].isdigit(), c
    A=AN
    v=A.index(w[0]); v=v*36+A.index(w[1]); v=v*10+int(w[2])
    for ch in w[3:]: v=v*27+(A.index(ch)-10)
    return v
def an50(c):
    # 11-char layout: 3 chars, slash flag, 3 chars, slash flag, 3 chars
    parts=c.split('/')
    w=[' ']*11
    if len(parts)==1: s=parts[0].ljust(9); w[0:3]=s[0:3]; w[4:7]=s[3:6]; w[8:11]=s[6:9]
    elif len(parts)==2 and len(parts[0])<=6 and len(parts[1])<=3:
        s=parts[0].ljust(6); w[0:3]=s[0:3]; w[4:7]=s[3:6]; w[7]='/'; w[8:11]=parts[1].ljust(3)
    else: raise ValueError(c)
    A=AN; v=A.index(w[0])
    for i,r in zip(range(1,11),[38,38,2,38,38,38,2,38,38,38]):
        v=v*r+((1 if w[i]=='/' else 0) if r==2 else A.index(w[i]))
    assert unpackAN50(v)==c,(c,unpackAN50(v))
    return v
def directed(fr,to,cmd,num=None,i3=0,pf=0,pt=0):
    extra=(pf<<7)|(pt<<6)|(0 if num is None else num+31)
    return frame(ib(3,3)+ib(call28(fr),28)+ib(call28(to),28)+ib(cmd,5)+ib(extra,8),i3)
def compound(typ,call,num16,bits3=0,i3=0):
    return frame(ib(typ,3)+ib(an50(call),50)+ib(num16,16)+ib(bits3,3),i3)
def grid(g):
    k=(ord(g[0])-65)*10+int(g[2]); m=(ord(g[1])-65)*10+int(g[3])
    v=(179-k)*180+m
    assert unpackGrid(v)==g
    return v
def huff(text,i3=0):
    b=[1,0]
    for ch in text: b+=[int(x) for x in HUFF[ch]]
    assert len(b)<=71,(text,len(b))
    b+=[0]+[1]*72
    return frame(b[:72],i3)
def codeword(index,sep):
    s=7;c=9
    out=ib(((index%s)<<1)+int(sep),5)
    x=index//s
    while x>0:
        x-=1; out=ib((x%c)+s,4)+out; x//=c
    return out
def jsc(items,i3=0):   # items: list of (index, sep)
    b=[1,1]
    for idx,sep in items: b+=codeword(idx,sep)
    assert len(b)<=71,len(b)
    b+=[0]+[1]*72
    return frame(b[:72],i3)
def widx(w):
    m=jmap()
    for k,(s,sz,_) in enumerate(m):
        if s==w: return k
    raise KeyError(w)
def ck16(s):
    crc=0
    for ch in s.encode('latin1'):
        crc^=ch
        for _ in range(8): crc=(crc>>1)^0x8408 if crc&1 else crc>>1
    A="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?"
    return A[crc//1681]+A[crc%1681//41]+A[crc%41]

# ---- driver ---------------------------------------------------------------------------------
FAILS=[]
def check(name, ok, detail=''):
    print('%-5s %s%s'%('ok' if ok else 'FAIL', name, (' - '+detail) if detail else ''))
    if not ok: FAILS.append(name)
def run(args, inp=''):
    r=subprocess.run([DUMP]+args,input=inp,capture_output=True,text=True)
    if r.returncode: print(r.stderr[-3000:]); raise SystemExit('rv_dump failed (sanitizer?) rc=%d'%r.returncode)
    return r.stdout.split('\n')
def build():
    os.makedirs(os.path.dirname(DUMP),exist_ok=True)
    core=['src/js8enc.c','src/js8crc.c','src/js8_constants.c','src/js8msg.c','src/js8jsc.c','src/js8asm.c']
    r=subprocess.run((os.environ.get('CC') or 'cc').split()+['-O1','-g','-fsanitize=address,undefined',os.path.join(HERE,'rv_dump.c')]+[os.path.join(JS8,c) for c in core]+['-o',DUMP],capture_output=True,text=True)
    if r.returncode: print(r.stderr); raise SystemExit('build failed')
def chars(B): return ''.join(A72[b2i(B[6*i:6*i+6])] for i in range(12))
def mk(B,i3):   # raw 87-bit frame as hex, CRC field left 0 (js8_msg_decode does not look at it)
    v=b2i(B); v=(v<<3)|i3; return '%022x'%(v<<13)
def t_encoder(n, rnd):
    cases=[(''.join(rnd.choice(ALPH[:64]) for _ in range(12)),rnd.randrange(8)) for _ in range(n)]
    cases+=[('0'*12,0),('+'*12,7),('-'*12,3)]
    out=run(['enc'],''.join('%s %d\n'%c for c in cases)); bad=0
    for (m,i3),l in zip(cases,out):
        mb,crc,tone=genjs8(m,i3)
        hx,c1,c2,k1,k2,tn=l.split()
        if hx!='%022x'%int(''.join(map(str,mb))+'0',2) or int(c1,16)!=crc or int(c2,16)!=crc or k1!='1' or k2!='1' or tn!=''.join(map(str,tone)): bad+=1
    check('1 encoder / CRC-12 / LDPC / tones vs genjs8.f90 model (%d frames)'%len(cases), bad==0, '%d mismatches'%bad)
    t=run(['tab']); nm=[l for l in t if l.startswith('NM')]; mn=[l for l in t if l.startswith('MN')]; b=0
    for j in range(87):
        f=nm[j].split(); row=[int(x) for x in f[1:8]]; r=int(f[9])
        if r!=nrw[j] or row[:r]!=NmT[j][:r] or any(row[r:]): b+=1
    for i in range(174):
        if [int(x) for x in mn[i].split()[1:]]!=MnT[i]: b+=1
    check('2 LDPC Nm / Mn / row weights vs bpdecode174.f90', b==0, '%d rows differ'%b)
def t_dict():
    import struct
    o=jmap(); h=open(os.path.join(SD,'JSCHEAD.BIN'),'rb').read()
    per=struct.unpack('<H',h[8:10])[0]; diff=[]
    for f in range((262144+per-1)//per):
        d=open(os.path.join(SD,'JSCD%03X.BIN'%f),'rb').read(); p=8
        for e in range(struct.unpack('<H',d[4:6])[0]):
            l=d[p]; w=d[p+1:p+1+l]; p+=1+l; idx=f*per+e
            if w!=o[idx][0][:o[idx][1]]: diff.append(idx)
    p=16
    for e in range(struct.unpack('<H',h[12:14])[0]):
        l=h[p]; w=h[p+1:p+1+l]; p+=1+l
        if w!=o[e][0][:o[e][1]]: diff.append(e)
    check('3 SD dictionary (JSCHEAD + JSCDxxx) = JSC::map[i].str[:size] for all 262144 words', not diff, str(diff[:5]))
def t_frames(N, rnd):
    def rb(n): return [rnd.getrandbits(1) for _ in range(n)]
    cases=[]
    for _ in range(N):
        t=rnd.randrange(12); i3=rnd.randrange(4)
        if t==0: B=rb(72); i3=rnd.randrange(8)
        elif t==1: B=ib(0,3)+rb(50)+ib(rnd.choice([rnd.randrange(65536),rnd.randrange(32390,32420),32400+32768,32399,0x8000]),16)+rb(3)
        elif t==2: B=ib(rnd.choice([1,2]),3)+rb(50)+ib(rnd.choice([rnd.randrange(65536),rnd.randrange(32390,32420),rnd.randrange(32410,32767),32766,32767]),16)+rb(3)
        elif t==3:
            call=lambda: rnd.choice([rnd.randrange(1<<28),rnd.randrange(NB-3,NB+58),rnd.randrange(NB)])
            B=ib(3,3)+ib(call(),28)+ib(call(),28)+rb(5)+rb(2)+ib(rnd.choice([0,rnd.randrange(64)]),6)
        elif t in (4,5,6,7,8):
            n=rnd.randrange(0,71); B=([1,0 if t<6 else 1]+rb(n)+[0]+[1]*72)[:72]
        elif t==9:
            n=rnd.randrange(0,73); B=(rb(n)+[0]+[1]*72)[:72]; i3=4|rnd.randrange(4)
        elif t==10:
            B=[1,1]
            while len(B)<72:
                x=rnd.choice([rnd.randrange(7),7,7,rnd.randrange(7,16)]); B+=ib(x,4)
                if x<7: B+=rb(1)
            B=B[:72]
        else:
            B=rnd.choice([[1]*72,[0]*72,[1,0]+[1]*70,[1,1]+[1]*70,[1,1,0]+[1]*69,[1,0,0]+[1]*69,[1,1]+[0]*70,[1,0]+[0]*70]); i3=rnd.randrange(8)
        cases.append((B,i3))
    out=run(['msg',SD],''.join(mk(B,i3)+'\n' for B,i3 in cases))
    st=collections.Counter(); ex={}
    for (B,i3),l in zip(cases,out):
        f=l.split('|'); got=bytes.fromhex(f[11]); typ=int(f[0]); exp=decode(chars(B),i3)
        if exp is None: st['skipped: JS8Call reads base[k>7] (undefined)']+=1; continue
        if got==exp: st['equal']+=1; continue
        if b'RA90' in exp and got==exp.replace(b'RA90',b''): key='KNOWN grid value 32400: JS8Call shows RA90, product shows no grid'
        elif typ==255 and (exp.startswith(b'/') or b': /' in exp): key='KNOWN directed call >= nbasecall: JS8Call shows a "/...." call, product shows the raw frame'
        elif typ==2 and exp.endswith(b'SNR -31 ') and got==exp.replace(b' -31',b''): key='KNOWN compound-directed cmd 25/29 without the number flag: JS8Call appends -31'
        else: key='UNEXPECTED difference, type %d'%typ
        st[key]+=1; ex.setdefault(key,(mk(B,i3),got,exp))
    for k,v in sorted(st.items()):
        print('      %7d  %s'%(v,k))
        if k in ex: print('               e.g. %s product %r JS8Call %r'%ex[k])
    check('4 frame -> text vs DecodedText model (%d frames)'%N, st['equal']+st['skipped: JS8Call reads base[k>7] (undefined)']==N, '%d frames differ'%(N-st['equal']-st['skipped: JS8Call reads base[k>7] (undefined)']))
def t_asm():
    L=[]
    def fr(hexs,f=1000,snr=-5,t=0): L.append('%s %d %d %d'%(hexs,f,snr,t))
    def w(s,sep=0): return (widx(s),sep)
    L.append('# S1'); L.append('CK 123456789')
    L.append('# S2'); fr(compound(1,'DL1ABC/P',grid('JO70'),0,1),t=0); fr(compound(2,'G4XYZ',32410+0x80+19,0,2),t=15)
    body='MEET AT 1900'
    L.append('# S3'); fr(directed('DL1ABC','OK1BT',9,None,1),t=100); fr(huff(' MEET AT ',0),t=115); fr(huff('1900 '+ck16(body),2),t=130)
    L.append('# S4'); fr(directed('DL1ABC','OK1BT',9,None,1),t=200); fr(huff(' MEET AT ',0),t=215); fr(huff('1900 000',2),t=230)
    L.append('# S5'); fr(jsc([w(b'HELLO'),(69,0),w(b'WORLD',1),(72,0)],3),t=300)
    L.append('# S6'); fr(compound(1,'DL1ABC/P',grid('JO70'),0,1),f=1500,t=400); fr(huff('HELLO',2),f=1500,t=415)
    long=('TEA TEA TEA TEA TEA '*14).rstrip(); full=long+' '+ck16(long)
    L.append('# S7'); fr(directed('DL1ABC','OK1BT',9,None,1),f=1800,t=500)
    ch=[full[i:i+20] for i in range(0,len(full),20)]
    for k,c in enumerate(ch): fr(huff(c,2 if k==len(ch)-1 else 0),f=1800,t=515+15*k)
    out=run(['asm',SD,'OK1BT'],'\n'.join(L)+'\n'); sec={}; cur=None
    for l in out:
        if l.startswith('# '): cur=l[2:]; sec[cur]=[]
        elif cur: sec[cur].append(l)
    msgs=lambda s:[l for l in sec[s] if l.startswith('  MSG ')]
    check('5.1 CRC-16/KERMIT checksum of "123456789" is 54G', sec['S1']==['CK 123456789 -> 54G'], str(sec['S1']))
    check('5.2 compound call + compound-directed -> "DL1ABC/P: G4XYZ SNR -12"', len(msgs('S2'))==1 and 'text=[DL1ABC/P: G4XYZ SNR -12]' in msgs('S2')[0], str(msgs('S2')))
    check('5.3 MSG with valid checksum: verified and stripped', len(msgs('S3'))==1 and 'flags=41' in msgs('S3')[0] and 'text=[DL1ABC: OK1BT MSG MEET AT 1900]' in msgs('S3')[0], str(msgs('S3')))
    check('5.4 MSG with wrong checksum is flagged', len(msgs('S4'))==1 and 'flags=81' in msgs('S4')[0], str(msgs('S4')))
    check('5.5 KNOWN JSC words 69 (newline) and 72 (0x1a) must not reach the message text raw (log lines)', not any('\\x0a' in l or '\\x1a' in l for l in msgs('S5')), str(msgs('S5')))
    check('5.6 KNOWN compound frame followed by data keeps the sender call (JS8Call shows "DL1ABC/P: HELLO")', any('DL1ABC/P' in l for l in msgs('S6')), str(msgs('S6')))
    check('5.7 KNOWN 279-character MSG with valid checksum is delivered whole and verified', len(msgs('S7'))==1 and 'flags=41' in msgs('S7')[0], msgs('S7')[0][:90] if msgs('S7') else '')
if __name__=='__main__':
    import argparse
    ap=argparse.ArgumentParser(); ap.add_argument('--n',type=int,default=300000); ap.add_argument('--seed',type=int,default=1)
    a=ap.parse_args(); rnd=random.Random(a.seed)
    build(); t_encoder(3000,rnd); t_dict(); t_frames(a.n,rnd); t_asm()
    print('%d check(s) failed'%len(FAILS) if FAILS else 'all checks passed')
    sys.exit(1 if FAILS else 0)
