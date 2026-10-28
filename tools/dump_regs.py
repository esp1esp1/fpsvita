#!/usr/bin/env python3
"""
Print the stopped thread, its registers and the module each address falls in from a
Vita crash dump (ux0:data/psp2core-*.psp2dmp, gzip-compressed ELF core). Enough to tell
whose code faulted without the full vita-parse-core tool. Usage: tools/dump_regs.py DUMP
"""
import gzip, struct, sys
d=gzip.decompress(open(sys.argv[1],'rb').read())
e_phoff,=struct.unpack_from('<I',d,28); e_phnum,=struct.unpack_from('<H',d,44)
notes={}
for i in range(e_phnum):
    p_type,p_off,_,_,p_filesz,_,_,_=struct.unpack_from('<IIIIIIII',d,e_phoff+32*i)
    if p_type!=4: continue
    off=p_off; end=p_off+p_filesz
    while off+12<=end:
        namesz,descsz,ntype=struct.unpack_from('<III',d,off)
        name=d[off+12:off+12+namesz].rstrip(b'\0').decode()
        doff=off+12+((namesz+3)&~3)
        notes[name]=d[doff:doff+descsz]
        off=doff+((descsz+3)&~3)
u32=lambda b,o: struct.unpack_from('<I',b,o)[0]
cstr=lambda b,o: b[o:b.index(b'\0',o)].decode('latin-1')
mods=[]; data=notes['MODULE_INFO']; num=u32(data,4); off=8
for _ in range(num):
    name=cstr(data,off+0x24); nseg=u32(data,off+0x4C); off+=0x50
    segs=[]
    for s in range(nseg):
        segs.append((u32(data,off+8),u32(data,off+12))); off+=0x14
    off+=0x10; mods.append((name,segs))
def where(a):
    for name,segs in mods:
        for i,(st,sz) in enumerate(segs):
            if st<=a<st+sz: return '%s+seg%d+0x%x'%(name,i,a-st)
    return '?'
threads={}; data=notes['THREAD_INFO']; num=u32(data,4); off=8
for _ in range(num):
    sz=u32(data,off); t=data[off:off+sz]
    threads[u32(t,4)]=dict(name=cstr(t,8),stop=u32(t,0x74),pc=u32(t,0x9C)); off+=sz
data=notes['THREAD_REG_INFO']; num=u32(data,4); off=8
for _ in range(num):
    sz=u32(data,off); t=data[off:off+sz]; tid=u32(t,4)
    threads.setdefault(tid,{})['gpr']=[u32(t,8+4*x) for x in range(16)]; off+=sz
print('modules:', ', '.join(n for n,_ in mods))
for tid,t in threads.items():
    if t.get('stop',0)==0: continue
    print('\nTHREAD %s tid=0x%x stop=0x%x pc=0x%08x %s'%(t.get('name'),tid,t['stop'],t['pc'],where(t['pc'])))
    g=t.get('gpr',[])
    if g:
        for i,v in enumerate(g): print('  r%-2d=0x%08x %s'%(i,v,where(v) if i in (12,13,14,15) or 0x81000000<=v<0x90000000 else ''))
print('\nnon-stopped threads:', ', '.join(t.get('name','?') for t in threads.values() if not t.get('stop')))
