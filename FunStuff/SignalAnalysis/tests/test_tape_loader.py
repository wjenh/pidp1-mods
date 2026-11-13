#!/usr/bin/env python3
"""Real installed CPU + Type 550 transport/control; no live emulator, no writes."""
import hashlib,json,os,re,struct,subprocess,sys,tempfile
from pathlib import Path
from test_core import ROOT,tape_memory,write_mem,read_mem,decode,reference
sys.path.insert(0,str(ROOT))
from tape import from_drum,make_image,validate_recording
from drum import calibration_image


def ascii_words(text,size):
    data=[ord(c) for c in text]+[0]
    if len(data)%2:data.append(0)
    words=[data[n]*512+data[n+1] for n in range(0,len(data),2)]
    assert len(words)<=size
    return words+[0]*(size-len(words))


def main():
    memory,entry=tape_memory(ROOT/'workstation-tape-test.rim')
    sy={f[2]:int(f[0],8) for line in (ROOT/'workstation-tape-test.sym').read_text().splitlines()
        if len(f:=line.split())==4 and re.fullmatch('[0-7]{6}',f[0])}
    with tempfile.TemporaryDirectory(prefix='saw-mt-') as td:
        p=Path(td);source=p/'cal.drm';source.write_bytes(calibration_image())
        image=p/'t.img';make_image(image,[('APOLLO11',source)],Path('/opt/pidp1-mods/bin/mtfs'))
        code=p/'code';patch=p/'patch';out=p/'out';dummy=p/'unused.drm';dummy.write_bytes(calibration_image())
        write_mem(code,memory)
        def run(name='APOLLO11',path=image,extra=None):
            edits={sy['tape_name']+n:w for n,w in enumerate(ascii_words(name,6))}
            write_mem(patch,edits)
            env=dict(os.environ,SAW_STOP_ON_FAULT='1',SAW_TEST_TAPE_IMAGE=str(path),**(extra or {}))
            result=subprocess.run([str(ROOT/'tests/tape_runner'),str(code),str(patch),str(out),f'{entry:o}',str(dummy)],capture_output=True,text=True,env=env)
            assert result.returncode==0,(result.stdout,result.stderr)
            return read_mem(out),result.stdout
        before=hashlib.sha256(image.read_bytes()).hexdigest()
        mem,metrics=run()
        diag={k:mem[sy[k]] for k in ('fault','done','ovfcnt','t_stage','t_status','input_source','ntrack')}
        assert diag['fault']==0 and diag['done']==1 and diag['t_stage']==9,(diag,metrics)
        raw=struct.unpack('<131072I',source.read_bytes())
        assert [mem[0o50000+n] for n in range(4096)]==list(raw[4096:8192])
        expected=reference([decode(w) for w in raw[4096:4352]])
        assert [(decode(mem[0o2000+k]),decode(mem[0o2400+k])) for k in range(256)]==expected
        assert [mem[0o12000+k]*(1<<17)+mem[0o12400+k] for k in range(129)]==[r*r+i*i for r,i in expected[:129]]
        assert hashlib.sha256(image.read_bytes()).hexdigest()==before
        report={'calibration':diag,'real_type550':True,'loaded_samples_checked':4096,'FFT_and_power_match':True,'image_unchanged':True,'metrics':metrics.strip()}
        mem,_=run(name='MISSING')
        assert mem[sy['fault']]==6 and mem[sy['t_status']]==0o13
        mem,_=run(path=p/'absent.img')
        assert mem[sy['fault']]==6 and not (p/'absent.img').exists()
        errors=['missing recording','missing image without blank-tape creation']
        base=bytearray(from_drum(source.read_bytes()))
        def corrupted(label,change,expected_fault,fix_header=False):
            data=list(struct.unpack(f'<{len(base)//4}I',base));change(data)
            if fix_header:
                data[12]=0;data[12]=(-sum(data[:256]))%((1<<18)-1)
            words=p/(label+'.words');words.write_bytes(struct.pack(f'<{len(data)}I',*data))
            test_image=p/(label+'.img')
            subprocess.run(['/opt/pidp1-mods/bin/mtfs','-i',str(test_image)],check=True)
            subprocess.run(['/opt/pidp1-mods/bin/mtfs','-a','-n','APOLLO11','-r','-f','words','-s',str(words),str(test_image)],check=True)
            got,_=run(path=test_image)
            assert got[sy['fault']]==expected_fault and got[sy['done']]==0,(label,got[sy['fault']],got[sy['t_stage']])
            errors.append(label)
        corrupted('wrong_contract',lambda w:w.__setitem__(2,8000),7,True)
        corrupted('oversized_RAM',lambda w:w.__setitem__(5,192),7,True)
        corrupted('bad_header_checksum',lambda w:w.__setitem__(12,w[12]^1),7)
        corrupted('bad_payload_checksum',lambda w:w.__setitem__(256,w[256]^1),8)
        corrupted('short_header',lambda w:w.__delitem__(slice(100,None)),8)
        corrupted('short_payload',lambda w:w.pop(),7)
        mem,_=run(extra={'SAW_INJECT_TAPE_MISS':'1','SAW_TAPE_STAGE_ADDR':f"{sy['t_stage']:o}"})
        assert mem[sy['fault']]==0 and mem[sy['done']]==1
        assert mem[sy['t_retry_total']]>=1
        assert [mem[0o50000+n] for n in range(4096)]==list(raw[4096:8192])
        warm,_=run(extra={'SAW_START_TAPE_BLOCK':'100'})
        assert warm[sy['fault']]==0 and [warm[0o50000+n] for n in range(4096)]==list(raw[4096:8192])
        report['warm_tape_reverse_seek_passed']=True
        report['injected_missed_word_recovered']=True
        report['retries_on_injection']=mem[sy['t_retry_total']]
        mem,_=run(extra={'SAW_INJECT_TAPE_MISS':'repeat','SAW_TAPE_STAGE_ADDR':f"{sy['t_stage']:o}",'SAW_TAPE_TRIES_ADDR':f"{sy['t_tries']:o}"})
        assert mem[sy['fault']]==6 and mem[sy['done']]==0 and mem[sy['t_retry_total']]==4
        report['persistent_read_error_stops_after_five_attempts']=True
        report['error_paths']=errors
    text=json.dumps(report,indent=2)+'\n';(ROOT/'tests/tape-results.json').write_text(text);print(text,end='')


if __name__=='__main__':main()
