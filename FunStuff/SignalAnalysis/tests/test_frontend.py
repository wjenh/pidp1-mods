#!/usr/bin/env python3
"""Exercise assembled acquisition/FFT/power/display-list generation without live I/O."""
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
from test_core import ROOT, tape_memory, write_mem, read_mem, decode, reference, trunc
import sys
sys.path.insert(0,str(ROOT))
from drum import calibration_image, validate_signal, WORD_MASK

def scene(mem,start):
    mode=0;x=y=0;scale=0;points=[];lines=[];chars=0
    for addr in range(start,start+1984):
        w=mem[addr];nxt=(w>>13)&7
        if mode==0:
            if w&0o2000:return {'words':addr-start+1,'points':points,'lines':lines,'characters':chars,'endpoint':[x,y]}
            if w&0o100:scale=(w>>4)&3
            mode=nxt
        elif mode==1:
            if w&0o200000:y=w&0o1777
            else:x=w&0o1777
            if w&0o2000:points.append([x,y])
            mode=nxt
        elif mode==3:
            for shift in (12,6,0):
                c=(w>>shift)&63
                if c==0o37:mode=0;break
                chars+=1
        elif mode==4:
            dx=(w&127)*(1<<scale)*(-1 if w&0o200 else 1)
            dy=((w>>8)&127)*(1<<scale)*(-1 if w&0o100000 else 1)
            nx,ny=x+dx,y+dy
            assert 0<=nx<=1023 and 0<=ny<=1023,('edge',addr,x,y,nx,ny)
            if w&0o200000:lines.append([x,y,nx,ny])
            x,y=nx,ny
            if w&0o400000:mode=0
        else:raise AssertionError(('unexpected mode',mode,addr))
    raise AssertionError('List exceeded allocation or has no HALT')

def main():
    memory,entry=tape_memory(ROOT/'workstation-test.rim')
    sym={f[2]:int(f[0],8) for line in (ROOT/'workstation-test.sym').read_text().splitlines()
         if len(f:=line.split())==4 and re.fullmatch('[0-7]{6}',f[0])}
    report={}
    with tempfile.TemporaryDirectory(prefix='saw-frontend-') as td:
        t=Path(td);image=t/'code.mem';inp=t/'patch.mem';out=t/'out.mem';disk=t/'disk.drm'
        write_mem(image,memory);write_mem(inp,{})
        def run(data,env=None):
            disk.write_bytes(data)
            r=subprocess.run([str(ROOT/'tests/frontend_runner'),str(image),str(inp),str(out),f'{entry:o}',str(disk)],
                             check=False,capture_output=True,text=True,env=env)
            assert r.returncode==0,(r.stdout,r.stderr)
            return read_mem(out),r.stdout
        raw=calibration_image()
        mem,metrics=run(raw)
        assert mem[sym['fault']]==0,(mem[sym['fault']],metrics)
        assert mem[sym['done']]==1 and mem[sym['ovfcnt']]==0,(mem[sym['done']],mem[sym['ovfcnt']],metrics)
        words=struct.unpack('<131072I',raw)
        samples=[decode(w) for w in words[4096:4352]]
        got=[(decode(mem[0o2000+k]),decode(mem[0o2400+k])) for k in range(256)]
        assert got==reference(samples)
        assert [mem[0o4000+n] for n in range(256)]==list(words[4096:4352])
        assert mem[sym['nframes']]==1
        active=mem[sym['active']];assert active==0o20100
        render=scene(mem,active)
        assert len(render['points'])==1
        assert render['points'][0]==[128,768]
        wave=[v for v in render['lines'] if min(v[1],v[3])>500 and v[0]>=128 and v[2]<=893 and v[2]-v[0]<=3]
        assert wave and wave[0][:2]==[128,768]
        for n in range(1,256):
            q=trunc(samples[n],2)
            endpoint=[128+3*n,768+trunc(q+trunc(q,2),512)]
            assert any(v[2:]==endpoint for v in wave),(n,endpoint)
        assert sum(v[2]-v[0] for v in wave)==3*255
        bars=[]
        for k in range(129):
            r,i=got[k];s=r*r+i*i
            assert mem[0o12000+k]*(1<<17)+mem[0o12400+k]==s
            h=(s>>17)>>(5 if k in (0,128) else 4)
            if h:
                bars.append((k,min(h,256)))
        assert [k for k,h in bars]==[32,64],bars
        for k,h in bars:
            matching=[v for v in render['lines'] if v[0]==v[2]==128+6*k and v[1]>=64]
            # Tick marks share these x positions; the matching full bar must start at the baseline.
            y=64
            while y<64+h:
                ends=[v[3] for v in matching if v[1]==y and y<v[3]<=64+h]
                assert ends,(k,h,y,matching)
                y=max(ends)
            assert y==64+h
        assert render['characters']>80
        report['calibration']={'bars':bars,'list_words':render['words'],'waveform_samples':256,'connected_vectors':len(wave),'metrics':metrics.strip()}
        # A corrupt sample track must stop before transforming or publishing it.
        corrupt=bytearray(raw);corrupt[4096*4+4]^=1
        mem,_=run(corrupt)
        assert mem[sym['fault']]==3 and mem[sym['done']]==0
        # A corrupt directory must also fail closed.
        corrupt=bytearray(raw);corrupt[4]^=1
        mem,_=run(corrupt)
        assert mem[sym['fault']]==3 and mem[sym['done']]==0
        mem,_=run(raw,dict(os.environ,SAW_TEST_DMA_ERROR='1'))
        assert mem[sym['fault']]==2 and mem[sym['done']]==0
        report['fault_paths']=['sample checksum','directory checksum','DMA status']
        # Eighteen published frames cross into track 2 and exercise both display buffers.
        memory,entry=tape_memory(ROOT/'workstation-sequence.rim')
        sym={f[2]:int(f[0],8) for line in (ROOT/'workstation-sequence.sym').read_text().splitlines()
             if len(f:=line.split())==4 and re.fullmatch('[0-7]{6}',f[0])}
        write_mem(image,memory)
        import math
        w=list(struct.unpack('<131072I',raw))
        for track in (1,2):
            for frame in range(16):
                k=(track-1)*16+frame+1
                for n in range(256):
                    v=round((1<<16)*math.sin(math.tau*k*n/256))
                    w[track*4096+frame*256+n]=v if v>=0 else WORD_MASK^-v
            pos=32+(track-1)*8
            w[pos:pos+8]=[track,(track-1)*4096,4096,sum(w[track*4096:(track+1)*4096])%WORD_MASK,0,0,0,0]
        w[4]=2;w[5]=8192;w[11]=0;w[11]=(-sum(w[:4096]))%WORD_MASK
        raw2=struct.pack('<131072I',*w);validate_signal(raw2)
        mem,metrics=run(raw2)
        assert mem[sym['done']]==1 and mem[sym['fault']]==0 and mem[sym['ovfcnt']]==0
        assert mem[sym['nframes']]==18 and mem[sym['track']]==2 and mem[sym['frame']]==1
        assert mem[sym['active']]==0o24000
        samples=[decode(v) for v in w[2*4096+256:2*4096+512]]
        got=[(decode(mem[0o2000+k]),decode(mem[0o2400+k])) for k in range(256)]
        assert got==reference(samples)
        render=scene(mem,mem[sym['active']])
        assert len(render['points'])==1 and mem[sym['nrender']]>18
        report['sequence']={'published_frames':18,'track':2,'frame':1,'active_list':'B','metrics':metrics.strip()}
        # Test the actual production idle loop: off means hold; S1 means run; one S2 pulse advances once.
        memory,entry=tape_memory(ROOT/'workstation.rim')
        sym={f[2]:int(f[0],8) for line in (ROOT/'workstation.sym').read_text().splitlines()
             if len(f:=line.split())==4 and re.fullmatch('[0-7]{6}',f[0])}
        write_mem(image,memory)
        control=[]
        for mode in (0,1,2):
            env=dict(os.environ,SAW_STOP_DELAY_NS='3000000000',SAW_SENSE_MODE=str(mode))
            mem,metrics=run(raw,env)
            assert mem[sym['fault']]==0
            n=mem[sym['nframes']]
            assert (n==1 if mode==0 else n>=3 if mode==1 else n==2),(mode,n,metrics)
            control.append({'sense_mode':mode,'published_frames':n})
        report['controls']=control
        # Force steep segments using alternating full-scale samples and verify list bounds/endpoints.
        memory,entry=tape_memory(ROOT/'workstation-test.rim');write_mem(image,memory)
        w=list(struct.unpack('<131072I',raw))
        for n in range(4096): w[4096+n]=131071 if n%2==0 else 0o400000
        w[35]=sum(w[4096:8192])%WORD_MASK;w[11]=0;w[11]=(-sum(w[:4096]))%WORD_MASK
        mem,_=run(struct.pack('<131072I',*w))
        assert mem[sym['fault']]==0
        render=scene(mem,mem[sym['active']])
        assert render['words']<1984 and len(render['points'])==1
        report['steep_waveform_list_words']=render['words']


    text=json.dumps(report,indent=2)+'\n'
    (ROOT/'tests/frontend-results.json').write_text(text)
    print(text,end='')

if __name__=='__main__':main()
