#!/usr/bin/env python3
"""Guest pen ISR + modal picker + real read-only Type 550, isolated from live Pi."""
import hashlib,json,os,re,struct,subprocess,tempfile,sys
from pathlib import Path
from test_core import ROOT,tape_memory,write_mem,read_mem,decode,reference
sys.path.insert(0,str(ROOT))
from tape import filesystem_tool,make_image


def main():
    image=ROOT/'samples/SignalStation.mtp'
    memory,entry=tape_memory(ROOT/'workstation-tape.rim')
    sy={f[2]:int(f[0],8) for line in (ROOT/'workstation-tape.sym').read_text().splitlines()
        if len(f:=line.split())==4 and re.fullmatch('[0-7]{6}',f[0])}
    before=hashlib.sha256(image.read_bytes()).hexdigest()
    tool=filesystem_tool()
    with tempfile.TemporaryDirectory(prefix='saw-picker-') as td:
        d=Path(td);code=d/'code';patch=d/'patch';out=d/'out';script=d/'pen'
        write_mem(code,memory);write_mem(patch,{})
        def run(events=None,delay=1000000000,tape=image):
            env=dict(os.environ,SAW_TEST_TAPE_IMAGE=str(tape))
            if events:
                script.write_text(''.join(f'{sy[name]:o} {value} {x} {y} {hold}\n' for name,value,x,y,hold in events))
                env.update(SAW_PEN_SCRIPT=str(script),SAW_PEN_STOP_DELAY_NS=str(delay))
            else:env['SAW_STOP_DELAY_NS']='100000000'
            result=subprocess.run([str(ROOT/'tests/tape_runner'),str(code),str(patch),str(out),f'{entry:o}',str(ROOT/'samples/SignalAnalysis.drm')],env=env,text=True)
            assert result.returncode==0,(result.stdout,result.stderr)
            return read_mem(out)
        baseline=run()
        def event(condition,value,x,y,hold=40):return condition,value,x,y,hold
        opened=[event('done',1,740,980,700)] # held LOAD FILE must not repeatedly open/close
        selected=opened+[event('p_modal',1,220,756)] # second row, TWOTONE
        cancelled=run(selected+[event('p_selected',2,780,180)])
        assert cancelled[sy['p_modal']]==0 and cancelled[sy['fault']]==0
        assert cancelled[sy['p_count']]==3 and cancelled[sy['p_selected']]==2
        assert all(cancelled[n]==baseline[n] for n in range(0o50000,0o200000))
        assert cancelled[sy['nframes']]==1
        loaded=run(selected+[event('p_selected',2,580,180,700),event('p_modal',0,2048,0,0)])
        assert loaded[sy['p_modal']]==0 and loaded[sy['fault']]==0 and loaded[sy['t_stage']]==9
        assert loaded[sy['ntrack']]==1 and loaded[sy['nframes']]==1
        raw=struct.unpack('<131072I',(ROOT/'samples/SignalAnalysis.drm').read_bytes())
        assert [loaded[n] for n in range(0o50000,0o60000)]==list(raw[4096:8192])
        expected=reference([decode(w) for w in raw[4096:4352]])
        assert [(decode(loaded[0o2000+k]),decode(loaded[0o2400+k])) for k in range(256)]==expected
        # Runtime filename bytes must be TWOTONE, independent of compiled APOLLO11.
        words=[loaded[sy['tape_name']+n] for n in range(5)]
        text=''.join(chr((w>>shift)&0x1ff) for w in words for shift in (9,0)).rstrip('\0 ')
        assert text=='TWOTONE',repr(text)
        # More than one page, full 9-character names, and a foreign data file.
        fixture=d/'pages.mtp'
        calibration=ROOT/'samples/SignalAnalysis.drm'
        make_image(fixture,[('APOLLO11',calibration)]+[(f'FILE{n:02d}',calibration) for n in range(1,9)]+[('NINECHARS',calibration)],tool)
        bad=d/'bad.words';bad.write_bytes(struct.pack('<256I',*([0]*256)))
        fixture.chmod(0o644)
        subprocess.run([str(tool),'-a','-n','BADFILE','-r','-f','words','-s',str(bad),str(fixture)],check=True)
        fixture.chmod(0o444)
        fixture_before=hashlib.sha256(fixture.read_bytes()).hexdigest()
        events=opened+[
            event('p_modal',1,360,180), # NEXT
            event('p_page',8,150,180),  # PREV
            event('p_page',0,360,180),  # NEXT again
            event('p_page',8,220,692),  # BADFILE, third row of page 2
            event('p_selected',11,580,180),
            event('p_error',63,780,180), # Cancel is refused after invalidating RAM
            event('p_error',63,220,756), # NINECHARS, second row of page 2
            event('p_selected',10,580,180),
            event('p_modal',0,2048,0,0)]
        recovered=run(events,tape=fixture)
        assert recovered[sy['p_count']]==11 and recovered[sy['p_page']]==8
        assert recovered[sy['p_modal']]==0 and recovered[sy['fault']]==0
        assert recovered[sy['p_invalid']]==0 and recovered[sy['t_stage']]==9
        assert [recovered[n] for n in range(0o50000,0o60000)]==list(raw[4096:8192])
        words=[recovered[sy['tape_name']+n] for n in range(5)]
        assert ''.join(chr((w>>shift)&511) for w in words for shift in (9,0)).rstrip('\0 ')=='NINECHARS'
        assert hashlib.sha256(fixture.read_bytes()).hexdigest()==fixture_before
        assert hashlib.sha256(image.read_bytes()).hexdigest()==before
        print(json.dumps({'cancel_preserves_RAM_and_spectrum':True,'runtime_name':'TWOTONE',
            'directory_files':3,'pagination_11_files':True,'nine_character_filename':True,
            'invalid_file_recovery_without_restart':True,'cancel_after_partial_load_refused':True,'held_press_debounced':True,'real_type550_reload':True,
            'FFT_matches_oracle':True,'tape_image_unchanged':True},indent=2))

if __name__=='__main__':main()
