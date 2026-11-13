#!/usr/bin/env python3
"""Load the tested workstation with a saved core snapshot; inspect live diagnostic words."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import socket
import struct
import subprocess
import time
from drum import DEFAULT_ROOT, DrumError, atomic_write, validate_signal

class Link:
    def __init__(self,port=1044):
        self.sock=socket.create_connection(('127.0.0.1',port),timeout=3)
        self.sock.settimeout(5)
        self.ident=0
        name=b'SignalAnalysis-status'
        reply=self.request(1,struct.pack('<III',1,0,len(name))+name)
        if len(reply)<28 or struct.unpack_from('<I',reply)[0]!=1:
            self.sock.close();raise DrumError('Unsupported debugger protocol.')
        self.request(0x62,struct.pack('<I',0))  # KEEP breakpoints/watches on disconnect.
    def close(self):self.sock.close()
    def __enter__(self):return self
    def __exit__(self,*args):self.close()
    def exact(self,n):
        data=b''
        while len(data)<n:
            block=self.sock.recv(n-len(data))
            if not block:raise DrumError('Debugger connection closed.')
            data+=block
        return data
    def request(self,kind,payload=b''):
        self.ident+=1
        self.sock.sendall(struct.pack('<IHHI',len(payload),kind,0,self.ident)+payload)
        while True:
            size,reply,status,ident=struct.unpack('<IHHI',self.exact(12))
            if size>262400:raise DrumError('Oversized debugger reply.')
            data=self.exact(size)
            if reply>=0xf000:continue
            if reply!=(kind|0x8000) or ident!=self.ident or status:
                raise DrumError(f'Debugger request failed (type {reply:x}, status {status}).')
            return data
    def state(self):
        data=self.request(0x10)
        if len(data)!=88:raise DrumError('Bad state reply.')
        return list(struct.unpack('<22I',data))
    def memory(self,addr,count):
        data=self.request(0x20,struct.pack('<II',addr,count))
        if len(data)!=4+count*4 or struct.unpack_from('<I',data)[0]!=count:
            raise DrumError('Bad memory reply.')
        return list(struct.unpack_from(f'<{count}I',data,4))

def symbols(home):
    result={}
    for line in (home/'workstation.sym').read_text().splitlines():
        f=line.split()
        if len(f)==4 and re.fullmatch('[0-7]{6}',f[0]):result[f[2]]=int(f[0],8)
    return result

def status(home):
    sy=symbols(home)
    with Link() as link:
        state=link.state()
        words=link.memory(0o100,3)
        ui=link.memory(0o10100,0o100)
        hi=link.memory(0o12000,129);lo=link.memory(0o12400,129)
        timing=link.memory(sy['display_go'],9)
        source=link.memory(sy['source_mode'],1)[0]
        stage=link.memory(sy['t_stage'],1)[0]
        tape_status=link.memory(sy['t_status'],1)[0]
        retries=link.memory(sy['t_retry_total'],1)[0]
    powers=[h*(1<<17)+l for h,l in zip(hi,lo)]
    peaks=sorted(range(129),key=powers.__getitem__,reverse=True)[:2]
    return {'running':bool(state[10]),'powered_on':bool(state[12]),'pc':f'{state[2]:06o}',
            'sense_switches_octal':f'{state[5]:02o}','done':words[0],'fft_overflows':words[1],'fault':words[2],
            'track':ui[sy['track']-0o10100],'frame':ui[sy['frame']-0o10100],
            'published_frames':ui[sy['nframes']-0o10100],
            'refresh_starts':ui[sy['nrender']-0o10100],
            'source':'tape' if source else 'drum','tape_stage':stage,'tape_status':tape_status,'tape_read_retries':retries,
            'display_gain':64 if ui[sy['shown_gain']-0o10100] else 4,
            'display_timing_ms':{'last':timing[4],'min':timing[5] if timing[8] else None,
                                 'max':timing[6],'over_25ms':timing[7],'completed_passes':timing[8]},
            'active_list':f"{ui[sy['active']-0o10100]:06o}",
            'largest_bins':[{'bin':k,'cps':16*k,'power_integer':powers[k]} for k in peaks]}

def launch(root, source="drum"):
    home=root/'FunStuff'/'SignalAnalysis'
    if source=='drum':validate_signal((root/'pdp23drum').read_bytes())
    else:
        from tape import selected_recording
        tape_config,tape_image,tape_info=selected_recording(root,home)
    with Link() as link:
        state=link.state()
        if state[10] or not state[12]:
            raise DrumError('Power must be on and the CPU halted before loading the workstation.')
        if state[13] or state[18] or state[19]:
            raise DrumError('Finish the prior debug session (single-step/breakpoints/watches) before loading.')
        memory=link.memory(0,65536)
        if link.state()[10]:raise DrumError('CPU started during snapshot; no program was loaded.')
    directory=home/'snapshots';directory.mkdir(exist_ok=True)
    tag=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    image=directory/(tag+'.mem');regs=directory/(tag+'.json')
    atomic_write(image,''.join(f'{a:06o}: {v:06o}\n' for a,v in enumerate(memory)).encode(),replace=False)
    atomic_write(regs,(json.dumps({'state_words':state,'memory':str(image)},indent=2)+'\n').encode(),replace=False)
    if source=='tape':
        from tape import select_drive
        select_drive(root,tape_image,tape_config['drive'])
    rim='workstation-tape.rim' if source=='tape' else 'workstation.rim'
    subprocess.run([str(root/'bin'/'fastload'),'-y',str(home/rim)],check=True)
    deadline=time.monotonic()+(180 if source=='tape' else 12)
    while True:
        report=status(home)
        if report['fault']:raise DrumError(f"Workstation fault {report['fault']}; tape stage {report['tape_stage']}, status {report['tape_status']:03o}. Check the recording and halt before reloading.")
        if report['done'] and report['published_frames']:
            report['previous_core_snapshot']=str(image)
            atomic_write(home/'live-results.json',(json.dumps(report,indent=2)+'\n').encode())
            return report
        if time.monotonic()>deadline:raise DrumError('No first frame before the loading timeout; inspect live status.')
        time.sleep(.5 if source=="tape" else .2)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root',type=Path,default=DEFAULT_ROOT)
    p.add_argument('--source',choices=['drum','tape'],default='drum')
    p.add_argument('command',choices=['status','run'])
    a=p.parse_args();root=a.root.resolve()
    try:
        r=launch(root,a.source) if a.command=='run' else status(root/'FunStuff'/'SignalAnalysis')
        print(json.dumps(r,indent=2))
    except (OSError,DrumError,subprocess.CalledProcessError) as e:
        p.exit(1,'live: '+str(e)+'\n')

if __name__=='__main__':main()
