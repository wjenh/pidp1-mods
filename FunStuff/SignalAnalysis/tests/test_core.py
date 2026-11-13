#!/usr/bin/env python3
"""Run assembled words using the installed CPU object; independent integer oracle."""
import cmath
import json
import math
from pathlib import Path
import random
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
D, MASK, N = 1 << 17, (1 << 18) - 1, 256

def encode(n):
    assert -D < n < D
    return n if n >= 0 else MASK ^ -n

def decode(w):
    return w if w < D else -(MASK ^ w)

def trunc(n, den):
    return (abs(n) // den) * (-1 if n < 0 else 1)

def reference(samples):
    # Pairs of integer codes keep every specified truncation explicit.
    a = [(trunc(samples[int(f'{i:08b}'[::-1],2)],2),0) for i in range(N)]
    for m in (2,4,8,16,32,64,128,256):
        for b in range(0,N,m):
            for j in range(m//2):
                k=j*N//m
                ar,ai=a[b+j]; br,bi=a[b+j+m//2]
                if k == 0:
                    tr,ti=br,bi
                elif k == 64:
                    tr,ti=bi,-br
                else:
                    c=math.trunc(D*math.cos(math.tau*k/N))
                    s=math.trunc(-D*math.sin(math.tau*k/N))
                    tr=trunc(br*c,D)-trunc(bi*s,D)
                    ti=trunc(br*s,D)+trunc(bi*c,D)
                ar,ai,tr,ti=(trunc(v,2) for v in (ar,ai,tr,ti))
                a[b+j]=(ar+tr,ai+ti)
                a[b+j+m//2]=(ar-tr,ai-ti)
    return a

def tape_memory(path, parts=False):
    """Decode the actual RIM bootstrap followed by AM1 address/end/data blocks."""
    holes=[b & 63 for b in path.read_bytes() if b & 128]
    assert len(holes)%3 == 0
    words=[(holes[i]<<12)|(holes[i+1]<<6)|holes[i+2] for i in range(0,len(holes),3)]
    mem={}
    i=0
    while words[i] != 0o607751:
        assert words[i]&0o770000 == 0o320000, 'Unexpected RIM bootstrap'
        addr=words[i]&0o7777
        assert addr not in mem
        mem[addr]=words[i+1]
        i+=2
    i+=1
    bootstrap=dict(mem)
    stream=words[i:]
    while True:
        begin=words[i];i+=1
        if begin & 0o400000:
            assert not begin & 0o200000, 'Expected START, not STOP'
            assert i==len(words), 'Trailing binary words'
            if parts: return bootstrap,stream,begin & 0o177777
            return mem, begin & 0o177777
        end=words[i];i+=1
        assert 0<=begin<end<=65536
        for addr in range(begin,end):
            assert addr not in mem, f'Overlapping tape block at {addr:o}'
            mem[addr]=words[i];i+=1

def listing_memory(path):
    pat=re.compile(r'^\s*\d+:\s+([0-7]{6})\s+([0-7]{6})(?:\s|$)')
    return {int(m[1],8):int(m[2],8) for line in path.read_text().splitlines() if (m:=pat.match(line))}

def write_mem(path, values):
    path.write_text(''.join(f'{a:06o}: {w:06o}\n' for a,w in sorted(values.items())))

def read_mem(path):
    return {int(a,8):int(w,8) for a,w in (line.split(':') for line in path.read_text().splitlines())}

def main():
    rng=random.Random(0x50445031)
    report={'target_cpu_object':'/opt/pidp1-mods/src/pdp1/pdp1.o',
            'pdp1d_extensions':False,'real_device_io':False,'cases':0}
    with tempfile.TemporaryDirectory(prefix='saw-core-') as td:
        temp=Path(td)
        image=temp/'program.mem'; inp=temp/'input.mem'; out=temp/'output.mem'
        def run(program, patches):
            memory, entry=tape_memory(ROOT/(program+'.rim'))
            for addr, word in listing_memory(ROOT/(program+'.lst')).items():
                assert memory[addr]==word, f'Tape/listing mismatch {addr:o}'
            write_mem(image,memory);write_mem(inp,patches)
            result=subprocess.run([str(ROOT/'tests/cpu_runner'),str(image),str(inp),str(out),f'{entry:o}'],
                                  check=True,capture_output=True,text=True)
            return read_mem(out),result.stdout.strip()
        mem,metrics=run('probe',{})
        assert mem[0o100]==1, 'Target arithmetic/extended-memory probe failed'
        assert mem[0o10400]==0o123456
        report['probe']=metrics
        cases={
            'zero':[0]*N,
            'positive_dc':[D-1]*N,
            'negative_dc':[-(D-1)]*N,
            'nyquist':[(D-1)*(-1)**n for n in range(N)],
            'impulse0':[D-1]+[0]*(N-1),
            'impulse17':[D-1 if n==17 else 0 for n in range(N)],
            'offbin':[round(.99*(D-1)*math.sin(math.tau*31.3*n/N)) for n in range(N)],
            'small':[rng.randint(-32,32) for _ in range(N)],
        }
        for k in range(129):
            cases[f'cosine{k}']=[round(.9*(D-1)*math.cos(math.tau*k*n/N)) for n in range(N)]
        for k in (1,7,32,63,64,100,127):
            cases[f'sine{k}']=[round(.9*(D-1)*math.sin(math.tau*k*n/N)) for n in range(N)]
        for i in range(10):
            cases[f'fullscale_noise{i}']=[rng.choice([-(D-1),D-1]) for _ in range(N)]
        max_error=0.; sample_metrics=None
        for name,samples in cases.items():
            raw=[encode(v) for v in samples]
            if name=='zero':raw=[MASK if n%2 else 0 for n in range(N)]
            patches={0o4000+n:v for n,v in enumerate(raw)}
            # Seed imaginary data: each invocation must clear it, not rely on initial zeros.
            patches.update({0o2400+n:rng.randrange(MASK+1) for n in range(N)})
            mem,metrics=run('core',patches)
            assert mem[0o100]==1, (name,'not complete')
            assert mem[0o101]==0, (name,'FFT overflow')
            assert [mem[0o4000+n] for n in range(N)]==raw,(name,'input changed')
            got=[(decode(mem[0o2000+k]),decode(mem[0o2400+k])) for k in range(N)]
            expected=reference(samples)
            assert got==expected, (name,next((k,a,b) for k,(a,b) in enumerate(zip(got,expected)) if a!=b))
            for k in range(129):
                r,i=got[k]
                hi,lo=mem[0o12000+k],mem[0o12400+k]
                assert 0<=hi<D and 0<=lo<D, (name,k,'invalid limb')
                assert hi*D+lo==r*r+i*i,(name,k,'power carry/low bits')
            if name in ('positive_dc','negative_dc','nyquist','impulse0','impulse17','offbin','small','sine32'):
                ideal=[sum(v*cmath.exp(-1j*math.tau*k*n/N) for n,v in enumerate(samples))/512 for k in range(N)]
                err=max(abs(complex(*a)-b) for a,b in zip(got,ideal))
                assert err<64,(name,'DFT error',err)
                max_error=max(max_error,err)
            if name.startswith('cosine') or name.startswith('sine'):
                k=int(re.sub(r'\D','',name))
                peak=max(range(129),key=lambda n:got[n][0]**2+got[n][1]**2)
                assert peak==k,(name,peak)
            report['cases']+=1
            if name=='offbin':sample_metrics=metrics
        # Execute AM1's actual extended-memory bootstrap on the target CPU. RPB's
        # external reader is a deterministic word source; all loader instructions run.
        bootstrap,stream,entry=tape_memory(ROOT/'core.rim',parts=True)
        write_mem(image,bootstrap)
        samples=cases['offbin']
        write_mem(inp,{0o4000+n:encode(v) for n,v in enumerate(samples)})
        tape=temp/'loader.words';tape.write_text(''.join(f'{w:06o}\n' for w in stream))
        result=subprocess.run([str(ROOT/'tests/cpu_runner'),str(image),str(inp),str(out),f'{entry:o}',str(tape)],
                              check=True,capture_output=True,text=True)
        mem=read_mem(out)
        got=[(decode(mem[0o2000+k]),decode(mem[0o2400+k])) for k in range(N)]
        assert got==reference(samples),'Executable AM1 loader/FFT mismatch'
        assert mem[0o100]==1 and mem[0o101]==0
        for k,(r,i) in enumerate(got[:129]):
            assert mem[0o12000+k]*D+mem[0o12400+k]==r*r+i*i
        report['extended_loader']=result.stdout.strip()

        # Direct power-only call with crafted low-limb carries and return to a HLT.
        symbols={fields[2]:int(fields[0],8) for line in (ROOT/'core.sym').read_text().splitlines()
                 if len(fields:=line.split())==4 and re.fullmatch('[0-7]{6}',fields[0])}
        patch={0o5000:0o620000|symbols['power'],0o5001:0o760400}
        carries=0
        for k in range(129):
            r,i=rng.randint(-45000,45000),rng.randint(-45000,45000)
            carries+=(r*r%D+i*i%D)>=D
            patch[0o2000+k]=encode(r);patch[0o2400+k]=encode(i)
        memory,_=tape_memory(ROOT/'core.rim')
        write_mem(image,memory);write_mem(inp,patch)
        subprocess.run([str(ROOT/'tests/cpu_runner'),str(image),str(inp),str(out),'5000'],
                       check=True,capture_output=True,text=True)
        mem=read_mem(out)
        for k in range(129):
            r,i=decode(patch[0o2000+k]),decode(patch[0o2400+k])
            assert mem[0o12000+k]*D+mem[0o12400+k]==r*r+i*i
        assert carries>0
        report['power_carry_cases']=carries
        report['max_checked_dft_error_counts']=round(max_error,6)
        report['example_fft_and_power_metrics']=sample_metrics
    result=json.dumps(report,indent=2)+'\n'
    (ROOT/'tests/results.json').write_text(result)
    print(result,end='')

if __name__=='__main__':
    main()
