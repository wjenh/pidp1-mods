#!/usr/bin/env python3
"""Verify every changing-calibration frame using the installed CPU + read-only adapters."""
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from test_core import ROOT, tape_memory, write_mem, read_mem, decode, reference
sys.path.insert(0,str(ROOT))
from drum import changing_calibration_image, changing_plan, validate_signal

def main():
    raw=changing_calibration_image();info=validate_signal(raw)
    memory,entry=tape_memory(ROOT/'workstation-sequence.rim')
    sy={f[2]:int(f[0],8) for line in (ROOT/'workstation-sequence.sym').read_text().splitlines()
        if len(f:=line.split())==4 and re.fullmatch('[0-7]{6}',f[0])}
    words=struct.unpack('<131072I',raw)
    with tempfile.TemporaryDirectory(prefix='saw-changing-') as directory:
        p=Path(directory);image=p/'code.mem';patch=p/'input.mem';out=p/'out.mem';disk=p/'changing.drm';trace=p/'frames.txt'
        write_mem(image,memory);write_mem(patch,{});disk.write_bytes(raw)
        env=dict(os.environ,SAW_FRAME_TRACE=str(trace),SAW_FRAME_COUNTER_ADDR=f"{sy['nframes']:o}")
        run=subprocess.run([str(ROOT/'tests/frontend_runner'),str(image),str(patch),str(out),f'{entry:o}',str(disk)],
                           env=env,check=True,capture_output=True,text=True)
        frames=[[int(v) for v in line.split()] for line in trace.read_text().splitlines()]
        assert len(frames)==18
        summaries=[]
        for index,values in enumerate(frames):
            frame=index%16
            assert values[0]==index+1 and len(values)==130
            samples=[decode(v) for v in words[4096+frame*256:4096+(frame+1)*256]]
            expected=reference(samples)
            powers=[r*r+i*i for r,i in expected[:129]]
            assert values[1:]==powers,(frame,'CPU power mismatch')
            plan=next(s for s in changing_plan() if s['first_frame']<=frame<=s['last_frame'])
            bins=[t['cps']//16 for t in plan['tones']]
            top=sorted(range(129),key=powers.__getitem__,reverse=True)[:len(bins)]
            assert set(top)==set(bins),(frame,top,bins)
            if frame in (0,4,8,12):
                summaries.append({'frame':frame,'peaks_cps':[k*16 for k in top],
                                  'bar_heights_x4':[(powers[k]>>17)>>4 for k in top]})
        mem=read_mem(out)
        assert mem[sy['fault']]==0 and mem[sy['ovfcnt']]==0
        assert mem[sy['frame']]==1 and mem[sy['nframes']]==18
    result={'image':info,'checked_frames':18,'wrap_verified':True,'scenes':summaries,'metrics':run.stdout.strip()}
    text=json.dumps(result,indent=2)+'\n'
    (ROOT/'tests/changing-results.json').write_text(text);print(text,end='')

if __name__=='__main__':main()
