#!/usr/bin/env python3
"""Execute the autonomous Type 340 list without CPU steps or interrupts.

This checks the assembled display control flow, SAVE/DDS returns, pacing and
coordinates. The real display and tape are checked on the Pi separately.
"""
import json,re
from test_core import ROOT,tape_memory
from display_cost import CHAR_STEPS


def counter_text(mem,sy,label):
    # Explicit shift, one decimal glyph, END. The Type 342 digits are codes 060..071.
    codes=[(mem[sy[label]+2*n+1]>>6)&63 for n in range(3)]
    return ''.join(chr(c) for c in codes)


def main():
    mem,_=tape_memory(ROOT/'workstation-tape.rim')
    sy={f[2]:int(f[0],8) for line in (ROOT/'workstation-tape.sym').read_text().splitlines()
        if len(f:=line.split())==4 and re.fullmatch('[0-7]{6}',f[0])}
    addr=sy['loading_scene'];mode=0;scale=0;x=y=0;saved=None;now=0;frames=[];writes=set()
    entries={sy[f'loading_line{n}']:n for n in range(4)}
    for _ in range(100000):
        if addr in entries:
            frames.append((entries[addr],now))
            if len(frames)==97:break
        w=mem[addr];addr+=1;now+=5000;nxt=(w>>13)&7
        if mode==0:
            assert not w&0o2000,'Autonomous spinner must not HALT or need a break'
            if w&0o100:scale=(w>>4)&3
            mode=nxt
        elif mode==1:
            if w&0o200000:y=w&0o1777
            else:x=w&0o1777;now+=35000
            mode=nxt
        elif mode==3:
            for shift in (12,6,0):
                c=(w>>shift)&63;now+=CHAR_STEPS[c]*1500+500
                if c==0o37:
                    assert saved is None,'Text END must not consume the animation return'
                    mode=0;break
        elif mode==4:
            dx=w&127;dy=(w>>8)&127
            now+=500+((1<<max(3,max(dx,dy).bit_length()))-1)*1500
            x+=dx*(1<<scale)*(-1 if w&0o200 else 1)
            y+=dy*(1<<scale)*(-1 if w&0o100000 else 1)
            if w&0o400000:mode=0
        elif mode==7:
            op=(w>>16)&3;target=w&0o17777;mode=nxt
            if op==3:saved=addr;addr=target
            elif op==2:addr=target
            elif op==1:
                assert saved is not None
                mem[target]=0o400000|saved;saved=None;writes.add(target)
            else:raise AssertionError('Invalid display subroutine word')
        else:raise AssertionError(f'Unexpected mode {mode}')
        assert 0<=x<=1023 and 0<=y<=1023,(x,y)
    assert [n for n,_ in frames[:96]]==[n for _ in range(2) for n in range(4) for _ in range(12)]
    assert writes=={sy['loading_return']},writes
    dwell=[(frames[n+12][1]-frames[n][1])/1e6 for n in range(0,85,12)]
    assert all(100<=ms<=250 for ms in dwell),dwell
    assert counter_text(mem,sy,'loading_count')=='000'
    print(json.dumps({'CPU_steps':0,'interrupts':0,'animation_frames':96,
        'orientation_order':[0,1,2,3],'nominal_orientation_ms':dwell,
        'display_writes_only_return_word':True,'all_coordinates_in_bounds':True},indent=2))

if __name__=='__main__':main()
