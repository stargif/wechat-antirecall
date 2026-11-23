#!/usr/bin/env python3
"""生成 270416 的 revoke-keeptip-content 补丁字节。
用法：python3 gen_patch.py <未打补丁的 arm64 切片> <输出 entries.json>"""
import struct,json,sys
d=open(sys.argv[1],'rb').read()
CAVE=0x16e00; SLOTP=0x9e27f00; SLOTF=0x9e27f08
def adrp(rd,pc,t):
    imm=((t>>12)-(pc>>12))&0x1fffff
    return 0x90000000|((imm&3)<<29)|((imm>>2)<<5)|rd
def ldr(rt,rn,off): return 0xF9400000|((off//8)<<10)|(rn<<5)|rt
def cbz(rt,off): return 0xB4000000|(((off//4)&0x7ffff)<<5)|rt
def br(rn): return 0xD61F0000|(rn<<5)
def b(pc,t,op=0x14000000):
    o=t-pc; assert abs(o)<(1<<27); return op|((o>>2)&0x3ffffff)
def stub(pc,slot,tail):
    w=[adrp(16,pc,slot),ldr(16,16,slot&0xfff),cbz(16,8),br(16)]
    return w+tail(pc+16)
s1=stub(CAVE,SLOTP,lambda pc:[b(pc,0x4666c28)])
orig_fin=struct.unpack_from('<I',d,0x4612e90)[0]
assert orig_fin==0x39496008,hex(orig_fin)   # ldrb w8,[x0,#0x258]
s2=stub(CAVE+0x20,SLOTF,lambda pc:[orig_fin,b(pc+4,0x4612e94)])
cave=struct.pack('<8I',*(s1+[0xD503201F]*(8-len(s1))))+struct.pack('<%dI'%len(s2),*s2)
name=b'@loader_path/libwxar_tip.dylib\0'
size=(24+len(name)+7)&~7
lc=struct.pack('<6I',0x80000018,size,24,2,0x10000,0x10000)+name
lc=lc.ljust(size,b'\0')
ncmds,szc=struct.unpack_from('<2I',d,0x10)
end=0x20+szc
assert not any(d[end:0x17000])
hdr=struct.pack('<2I',ncmds+1,szc+size)
e=[]
def add(a,new,old=None):
    old=old or d[a:a+len(new)]
    e.append(dict(arch='arm64',addr=hex(a),expected=[old.hex()],asm=new.hex()))
add(0x4666e98,bytes.fromhex('40100034'))
add(0x4667638,bytes.fromhex('7fe600f9'))
# 写入顺序：桩 → load command → 头 → 跳转，中途中断也不会留下畸形头
add(CAVE,cave); add(end,lc); add(0x10,hdr)
add(0x4666ba4,struct.pack('<I',b(0x4666ba4,CAVE,0x94000000)))
add(0x4612e90,struct.pack('<I',b(0x4612e90,CAVE+0x20)))
assert CAVE+0x30==0x16e30
json.dump(e,open(sys.argv[2],'w'),indent=1)
for x in e: print(x['addr'],x['expected'][0][:32],'->',x['asm'][:64])
