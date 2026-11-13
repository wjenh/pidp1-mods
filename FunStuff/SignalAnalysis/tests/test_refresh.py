#!/usr/bin/env python3
"""Interrupt refresh, clock wrap, slow-frame reporting, and both SBS modes."""
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path
from test_core import ROOT,tape_memory,write_mem,read_mem
import sys
sys.path.insert(0,str(ROOT))
from drum import calibration_image


def main():
    memory,entry=tape_memory(ROOT/'workstation.rim')
    sy={f[2]:int(f[0],8) for line in (ROOT/'workstation.sym').read_text().splitlines()
        if len(f:=line.split())==4 and re.fullmatch('[0-7]{6}',f[0])}
    report=[]
    with tempfile.TemporaryDirectory(prefix='saw-refresh-') as td:
        p=Path(td);image=p/'code';patch=p/'patch';out=p/'out';disk=p/'disk'
        write_mem(image,memory);write_mem(patch,{})
        disk.write_bytes(calibration_image())
        for name,extra in [('single',{}),('SBS16',{'SAW_TEST_SBS16':'1'}),
                           ('clock_wrap',{'SAW_CLOCK_OFFSET_MS':'59990'}),
                           ('slow_display',{'SAW_DISPLAY_DELAY_NS':'35000000'})]:
            env=dict(os.environ,SAW_STOP_DELAY_NS='3000000000',SAW_SENSE_MODE='1',**extra)
            r=subprocess.run([str(ROOT/'tests/frontend_runner'),str(image),str(patch),str(out),f'{entry:o}',str(disk)],
                             env=env,check=True,capture_output=True,text=True)
            mem=read_mem(out)
            assert mem[sy['fault']]==0 and mem[sy['ovfcnt']]==0
            lo,hi,n,late=[mem[sy[k]] for k in ('display_min','display_max','display_count','display_late')]
            assert mem[sy['nframes']]>=3 and n>=60
            if name=='slow_display':assert 35<=lo<=hi<=37 and late==n,(lo,hi,n,late)
            else:assert 5<=lo<=hi<=7 and late==0,(lo,hi,n,late)
            report.append({'case':name,'min_ms':lo,'max_ms':hi,'completed_passes':n,'over_25ms':late})
    text=json.dumps(report,indent=2)+'\n';(ROOT/'tests/refresh-results.json').write_text(text);print(text,end='')


if __name__=='__main__':main()
