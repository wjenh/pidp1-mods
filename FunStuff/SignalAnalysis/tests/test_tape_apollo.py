#!/usr/bin/env python3
"""Actual Type 550 loading of all Apollo samples and 146 oracle-checked spectra."""
import hashlib,json,os,re,struct,subprocess,tempfile
from pathlib import Path
from test_core import ROOT,tape_memory,write_mem,read_mem,decode,reference
import sys
sys.path.insert(0,str(ROOT))
from tape import make_image


def main():
    memory,entry=tape_memory(ROOT/'workstation-tape-sequence.rim')
    sy={f[2]:int(f[0],8) for line in (ROOT/'workstation-tape-sequence.sym').read_text().splitlines()
        if len(f:=line.split())==4 and re.fullmatch('[0-7]{6}',f[0])}
    source=ROOT/'samples/Apollo11.drm';words=struct.unpack('<131072I',source.read_bytes())
    with tempfile.TemporaryDirectory(prefix='saw-mt-apollo-') as td:
        p=Path(td);image=p/'speech.img';make_image(image,[('APOLLO11',source)],Path('/opt/pidp1-mods/bin/mtfs'))
        code=p/'code';patch=p/'patch';out=p/'out';trace=p/'trace';write_mem(code,memory);write_mem(patch,{})
        before=hashlib.sha256(image.read_bytes()).hexdigest()
        env=dict(os.environ,SAW_TEST_TAPE_IMAGE=str(image),SAW_GAIN_MODE='1',SAW_STOP_ON_FAULT='1',SAW_FRAME_TRACE=str(trace),SAW_FRAME_COUNTER_ADDR=f"{sy['nframes']:o}")
        result=subprocess.run([str(ROOT/'tests/tape_runner'),str(code),str(patch),str(out),f'{entry:o}',str(source)],env=env,capture_output=True,text=True,check=True)
        mem=read_mem(out)
        diag={k:mem[sy[k]] for k in ('fault','done','ovfcnt','t_stage','t_status','nframes','ntrack')}
        assert diag['fault']==0 and diag['nframes']==146,(diag,result.stdout,result.stderr)
        assert [mem[0o50000+n] for n in range(9*4096)]==list(words[4096:10*4096])
        frames=[[int(v) for v in line.split()] for line in trace.read_text().splitlines()]
        assert len(frames)==146
        for index,row in enumerate(frames):
            frame=index%144;samples=[decode(v) for v in words[4096+frame*256:4096+(frame+1)*256]]
            assert row[0]==index+1
            assert row[1:]==[r*r+i*i for r,i in reference(samples)[:129]],index
        assert hashlib.sha256(image.read_bytes()).hexdigest()==before
        assert 'drum_reads=0 ' in result.stdout,result.stdout
    report={'real_type550':True,'loaded_samples_checked':36864,'oracle_frames':146,'all_nine_RAM_fields_checked':True,'padding_and_wrap_checked':True,'no_drum_reads':True,'image_unchanged':True,'diagnostics':diag,'metrics':result.stdout.strip()}
    text=json.dumps(report,indent=2)+'\n';(ROOT/'tests/tape-apollo-results.json').write_text(text);print(text,end='')


if __name__=='__main__':main()
