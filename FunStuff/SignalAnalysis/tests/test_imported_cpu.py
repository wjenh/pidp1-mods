#!/usr/bin/env python3
"""Check all 144 imported Apollo frames plus two wrap frames against the integer oracle."""
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from test_core import ROOT,tape_memory,write_mem,read_mem,decode,reference
sys.path.insert(0,str(ROOT))
from drum import validate_signal

def main(path):
    disk=Path(path).resolve();raw=disk.read_bytes();info=validate_signal(raw)
    assert info['sample_tracks']==9,'This acceptance test expects the nine-track NASA example.'
    memory,entry=tape_memory(ROOT/'workstation-wav-test.rim')
    sy={f[2]:int(f[0],8) for line in (ROOT/'workstation-wav-test.sym').read_text().splitlines()
        if len(f:=line.split())==4 and re.fullmatch('[0-7]{6}',f[0])}
    words=struct.unpack('<131072I',raw)
    with tempfile.TemporaryDirectory(prefix='saw-wav-cpu-') as directory:
        p=Path(directory);image=p/'code.mem';patch=p/'input.mem';out=p/'out.mem';trace=p/'frames.txt'
        write_mem(image,memory);write_mem(patch,{})
        env=dict(os.environ,SAW_FRAME_TRACE=str(trace),SAW_FRAME_COUNTER_ADDR=f"{sy['nframes']:o}")
        run=subprocess.run([str(ROOT/'tests/wav_runner'),str(image),str(patch),str(out),f'{entry:o}',str(disk)],
                           env=env,check=True,capture_output=True,text=True)
        frames=[[int(v) for v in line.split()] for line in trace.read_text().splitlines()]
        assert len(frames)==146
        heights=[]
        for index,values in enumerate(frames):
            frame=index%144
            assert values[0]==index+1
            samples=[decode(v) for v in words[4096+frame*256:4096+(frame+1)*256]]
            expected=reference(samples)
            powers=[r*r+i*i for r,i in expected[:129]]
            assert values[1:]==powers,(frame,'CPU power mismatch')
            shift=0 if os.environ.get('SAW_GAIN_MODE')=='1' else 4
            heights.append(max(min(256,(v>>17)>>(shift+(k in (0,128)))) for k,v in enumerate(powers)))
        mem=read_mem(out)
        assert mem[sy['fault']]==0 and mem[sy['ovfcnt']]==0
        assert mem[sy['track']]==1 and mem[sy['frame']]==1
    result={'image':info,'checked_frames':146,'all_nine_tracks_checked':True,'partial_tail_and_wrap_checked':True,
            'display_gain':64 if os.environ.get('SAW_GAIN_MODE')=='1' else 4,
            'max_plot_height':max(heights),'nonzero_plot_frames':sum(h>0 for h in heights),'metrics':run.stdout.strip()}
    text=json.dumps(result,indent=2)+'\n';(ROOT/'tests/wav-cpu-results.json').write_text(text);print(text,end='')

if __name__=='__main__':main(sys.argv[1])
