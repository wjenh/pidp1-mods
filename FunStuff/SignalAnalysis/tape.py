#!/usr/bin/env python3
"""Read-only Signal Station recording containers and exclusive Microtape preparation."""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
from drum import D, WORD_MASK, DrumError, atomic_write, image_from_samples, validate_signal, DEFAULT_ROOT, Runtime

MAGIC=0o615327
VERSION=2
HEADER_WORDS=256
MAX_SECONDS=11
DEFAULT_IMAGE='../FunStuff/SignalAnalysis/samples/SignalStation.mtp'
DEFAULT_CONFIG={'name':'APOLLO11','drive':8,'image':DEFAULT_IMAGE}


def validate_recording(data, max_seconds=MAX_SECONDS):
    if len(data)%4 or len(data)<1024:raise DrumError('Truncated recording header.')
    words=list(struct.unpack(f'<{len(data)//4}I',data));h=words[:256];payload=words[256:]
    if any(w>WORD_MASK for w in words):raise DrumError('Recording has a word wider than 18 bits.')
    if h[:4]!=[MAGIC,VERSION,4096,256] or h[6:10]!=[17,1,8,0]:raise DrumError('Unsupported recording format or arithmetic contract.')
    if sum(h)%WORD_MASK or any(h[13:]):raise DrumError('Recording header checksum/reserved words are invalid.')
    frames=h[5]
    if not frames or frames%16 or frames>max_seconds*16:raise DrumError(f'Recording must occupy 1–{max_seconds} logical seconds.')
    expected=frames*256
    if len(payload)!=expected or not expected-4096<h[4]<=expected:raise DrumError('Recording length/valid sample count is inconsistent.')
    if any(payload[h[4]:]):raise DrumError('Recording tail padding must be zero.')
    if sum(payload)%WORD_MASK!=h[11]:raise DrumError('Recording payload checksum mismatch.')
    return {'format':'Signal Station recording v2','valid_samples':h[4],'seconds':h[4]/4096,
            'sample_tracks':frames//16,'frame_slots':frames,'signal_id':h[10],'words':len(words)}


def from_drum(data):
    info=validate_signal(data)
    if info['sample_tracks']>MAX_SECONDS:raise DrumError('RAM tape loading currently supports at most 11 seconds.')
    w=list(struct.unpack('<131072I',data));payload=w[4096:4096*(1+info['sample_tracks'])]
    h=[0]*256
    h[:13]=[MAGIC,VERSION,4096,256,info['valid_samples'],info['frame_slots'],17,1,8,0,info['signal_id'],sum(payload)%WORD_MASK,0]
    h[12]=(-sum(h))%WORD_MASK
    result=struct.pack(f'<{len(h)+len(payload)}I',*(h+payload));validate_recording(result)
    return result


def from_samples(samples,signal_id=3):return from_drum(image_from_samples(samples,signal_id))


def config(name='APOLLO11',drive=8,image=DEFAULT_IMAGE):
    name=name.upper()
    if not re.fullmatch(r'[A-Z0-9_.-]{1,9}',name):raise DrumError('Recording names use 1–9 letters, digits, dot, underscore or hyphen.')
    if not 1<=drive<=8:raise DrumError('Tape drive must be 1–8.')
    if not image or len(image)>110 or any(ord(c)<32 or ord(c)>126 or c in '\"\\,' for c in image):raise DrumError('Image path must be printable ASCII, at most 110 characters, without quotes, commas or backslashes.')
    return {'name':name,'drive':drive,'image':image}


def write_config(home, **kwargs):
    c=config(**kwargs);dest=Path(home)/'generated';dest.mkdir(exist_ok=True)
    (dest/'tape-config.json').write_text(json.dumps(c,indent=2)+'\n')
    text='// Fixed-size configuration; symbols remain identical in drum and tape builds.\n'
    text+=f'tape_drive, 0d{c["drive"]}\ntape_name,\n    ascii "{c["name"]}"\n    0\n    table 6-(.-tape_name), 0\n'
    text+='// Tape image selection is host-side; the guest deliberately issues no MMT.\n'
    (dest/'tape-config.ac').write_text(text)
    return c


def make_image(output, recordings, tool):
    output=Path(output).resolve()
    if output.exists():raise DrumError('Tape output exists; choose a new image name.')
    if not recordings:raise DrumError('Supply at least one NAME=drum.drm recording.')
    names=[]
    for name,_ in recordings:
        name=config(name=name)['name']
        if name in names:raise DrumError('Duplicate recording name: '+name)
        names.append(name)
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='saw-tape-') as td:
        p=Path(td);image=p/'new.img'
        subprocess.run([str(tool),'-i',str(image)],check=True)
        reports=[]
        for name,(_,source) in zip(names,recordings):
            original=Path(source).read_bytes()
            data=from_drum(original) if len(original)==524288 else original
            validate_recording(data)
            wordfile=p/(name+'.words');wordfile.write_bytes(data)
            subprocess.run([str(tool),'-a','-n',name,'-r','-f','words','-s',str(wordfile),str(image)],check=True)
            check=p/(name+'.check')
            subprocess.run([str(tool),'-x','-n',name,'-f','words','-o',str(check),str(image)],check=True)
            if check.read_bytes()!=data:raise DrumError('Microtape verification failed.')
            reports.append({'name':name,**validate_recording(data)})
        atomic_write(output,image.read_bytes(),replace=False,mode=0o444)
    return {'image':str(output),'recordings':reports,'read_only':True}



def selected_recording(root, home):
    c=config(**json.loads((Path(home)/'generated/tape-config.json').read_text()))
    image=(Path(root)/'Microtapes'/c['image']).resolve()
    if not image.is_file():raise DrumError('Tape image does not exist: '+str(image))
    with tempfile.TemporaryDirectory(prefix='saw-tape-inspect-') as td:
        words=Path(td)/'recording.words'
        result=subprocess.run([str(Path(root)/'bin/mtfs'),'-x','-n',c['name'],'-f','words','-o',str(words),str(image)],capture_output=True,text=True)
        if result.returncode:raise DrumError('Cannot read selected tape recording: '+result.stderr.strip())
        report=validate_recording(words.read_bytes())
    return c,image,report


def select_drive(root, image, drive):
    root=Path(root).resolve();image=Path(image).resolve(strict=True)
    Runtime(root).require_stopped()
    listing=root/'microtapes.txt'
    before=listing.read_text() if listing.exists() else ''
    lines=before.splitlines(keepends=True)
    matches=[n for n,line in enumerate(lines) if re.match(r'^\s*'+str(drive)+r'\s+',line)]
    if len(matches)>1:raise DrumError('Duplicate assignments for the selected tape drive; fix microtapes.txt first.')
    replacement=f'{drive} {image},locked\n'
    if matches:
        current=lines[matches[0]].split(None,1)[1].strip().split(',')[0]
        prior=(root/'Microtapes'/current).resolve()
        if prior!=image:raise DrumError(f'Drive {drive} already has another image assigned: {prior}. Choose a free drive.')
        lines[matches[0]]=replacement
    else:
        if lines and not lines[-1].endswith('\n'):lines[-1]+='\n'
        lines.append(replacement)
    after=''.join(lines)
    if after!=before:
        from datetime import datetime, timezone
        directory=root/'FunStuff/SignalAnalysis/snapshots';directory.mkdir(exist_ok=True,parents=True)
        backup=directory/('microtapes-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')+'.txt')
        atomic_write(backup,before.encode(),replace=False)
        if listing.exists() and listing.read_text()!=before:raise DrumError('Tape assignments changed during selection; try again.')
        atomic_write(listing,after.encode())
    return {'drive':drive,'image':str(image),'write_locked':True}


def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='command',required=True)
    q=sub.add_parser('configure');q.add_argument('--name',default='APOLLO11');q.add_argument('--drive',type=int,default=8);q.add_argument('--image',default=DEFAULT_IMAGE)
    q=sub.add_parser('pack');q.add_argument('output',type=Path);q.add_argument('--recording',action='append',required=True);q.add_argument('--tool',type=Path,default=DEFAULT_ROOT/'bin/mtfs')
    q=sub.add_parser('inspect');q.add_argument('words',type=Path)
    a=p.parse_args()
    try:
        if a.command=='configure':result=write_config(Path(__file__).resolve().parent,name=a.name,drive=a.drive,image=a.image)
        elif a.command=='inspect':result=validate_recording(a.words.read_bytes())
        else:
            pairs=[]
            for value in a.recording:
                if '=' not in value:raise DrumError('Use --recording NAME=/path/to/drum.drm')
                pairs.append(value.split('=',1))
            result=make_image(a.output,pairs,a.tool)
        print(json.dumps(result,indent=2))
    except (OSError,ValueError,DrumError,subprocess.CalledProcessError) as e:p.exit(1,'tape: '+str(e)+'\n')


if __name__=='__main__':main()
