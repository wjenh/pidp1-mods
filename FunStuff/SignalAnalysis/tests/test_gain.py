#!/usr/bin/env python3
"""Check gain-only rendering against unchanged FFT/power and held-frame controls."""
import json
import math
import os
from pathlib import Path
import random
import re
import subprocess
import sys
import tempfile
from test_core import ROOT, tape_memory, write_mem, read_mem
from test_frontend import scene
sys.path.insert(0, str(ROOT))
from drum import image_from_samples


def symbols(name):
    return {f[2]:int(f[0],8) for line in (ROOT/name).read_text().splitlines()
            if len(f:=line.split())==4 and re.fullmatch('[0-7]{6}',f[0])}


def main():
    report={}
    with tempfile.TemporaryDirectory(prefix='saw-gain-') as td:
        p=Path(td);code=p/'code';patch=p/'patch';out=p/'out';disk=p/'disk'
        write_mem(patch,{})
        def run(samples, high=False, hold_ns=None):
            name='workstation' if hold_ns is not None else 'workstation-test'
            memory,entry=tape_memory(ROOT/(name+'.rim'))
            sy=symbols(name+'.sym');write_mem(code,memory)
            disk.write_bytes(image_from_samples(samples,signal_id=9))
            env=dict(os.environ,SAW_GAIN_MODE=str(2 if hold_ns is not None else int(high)))
            if hold_ns is not None:env['SAW_STOP_DELAY_NS']=str(hold_ns)
            r=subprocess.run([str(ROOT/'tests/frontend_runner'),str(code),str(patch),str(out),f'{entry:o}',str(disk)],
                             capture_output=True,text=True,env=env,check=True)
            mem=read_mem(out)
            assert mem[sy['fault']]==0 and mem[sy['ovfcnt']]==0,r.stdout
            render=scene(mem,mem[sy['active']])
            selected=mem[sy['shown_gain']]
            label='gain64' if selected else 'gain4'
            expected=[memory[a] for a in range(sy[label],sy[label+'end'])]
            active=[mem[a] for a in range(mem[sy['active']],mem[sy['active']]+render['words'])]
            assert any(active[i:i+len(expected)]==expected for i in range(len(active)))
            for k in range(129):
                hi=mem[0o12000+k]
                h=hi>>(0 if selected else 4)
                if k in (0,128):h>>=1
                h=min(h,256)
                if h:
                    x=128+6*k
                    bars=[v for v in render['lines'] if v[0]==v[2]==x and 64<=v[1]<320 and v[3]>v[1]]
                    assert any(v[1]==64 for v in bars),(k,h)
                    y=64
                    while y<64+h:
                        ends=[v[3] for v in bars if v[1]==y and y<v[3]<=64+h]
                        assert ends,(k,h,y,bars)
                        y=max(ends)
                    assert y==64+h
            return mem,sy,render
        quiet=[round(2048*math.sin(math.tau*32*n/256)) for n in range(256)]
        low,ls,lr=run(quiet);high,hs,hr=run(quiet,True)
        for start,count in ((0o2000,512),(0o12000,129),(0o12400,129)):
            assert [low[start+k] for k in range(count)]==[high[start+k] for k in range(count)]
        assert low[ls['shown_gain']]==0 and high[hs['shown_gain']]==1
        qhi=high[0o12000+32]
        assert qhi>>4==0 and qhi>0
        report['quiet_tone']={'X4_height':qhi>>4,'X64_height':qhi,'FFT_and_power_unchanged':True}
        cases={'dc':[131071]*256,'nyquist':[131071 if n%2==0 else -131071 for n in range(256)],
               'loud_tone':[round(131071*math.sin(math.tau*32*n/256)) for n in range(256)]}
        rng=random.Random(314159)
        for i in range(12):cases['noise_'+str(i)]=[rng.randrange(-131071,131072) for _ in range(256)]
        sizes={}
        for name,samples in cases.items():
            mem,sy,render=run(samples,True)
            assert render['words']<1984
            sizes[name]=render['words']
        report['high_gain_list_words']=sizes
        checks=[]
        for delay,expected in ((600000000,1),(3000000000,0)):
            mem,sy,render=run(quiet,hold_ns=delay)
            assert mem[sy['shown_gain']]==expected
            assert mem[sy['nframes']]==1
            for start,count in ((0o2000,512),(0o12000,129),(0o12400,129)):
                assert [mem[start+k] for k in range(count)]==[low[start+k] for k in range(count)]
            checks.append({'gain':64 if expected else 4,'published_frames':mem[sy['nframes']]})
        report['held_frame_switching']=checks
    text=json.dumps(report,indent=2)+'\n'
    (ROOT/'tests/gain-results.json').write_text(text);print(text,end='')


if __name__=='__main__':main()
