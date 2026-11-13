#!/usr/bin/env python3
"""Create, validate, select and restore workstation drum images. Python stdlib only."""
import argparse
import contextlib
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import socket
import stat
import struct
import sys
import tempfile
import time
import uuid

WORDS_PER_TRACK = 4096
TRACKS = 32
WORD_MASK = (1 << 18) - 1
D = 1 << 17
IMAGE_BYTES = TRACKS * WORDS_PER_TRACK * 4
MAGIC = 0o615327
DEFAULT_ROOT = Path('/opt/pidp1-mods')

class DrumError(Exception):
    pass

def digest(data):
    return hashlib.sha256(data).hexdigest()

def unpack(data, full=False):
    if not data or len(data) % 4 or len(data) > IMAGE_BYTES or (full and len(data) != IMAGE_BYTES):
        raise DrumError('Expected a drum of up to 32 tracks in 32-bit little-endian word slots.')
    words = list(struct.unpack(f'<{len(data)//4}I', data))
    if any(w > WORD_MASK for w in words):
        raise DrumError('Image contains values outside the 18-bit word range.')
    return words

def validate_signal(data):
    w = unpack(data, full=True)
    expected = {0:MAGIC, 1:1, 2:4096, 3:256, 6:17, 7:1, 8:8, 9:0}
    if any(w[k] != v for k,v in expected.items()):
        raise DrumError('Workstation header does not match the frozen sampling/numeric contract.')
    ntracks, total = w[4], w[5]
    if not 1 <= ntracks <= 31 or not 1 <= total <= ntracks*4096:
        raise DrumError('Invalid recording length.')
    if any(w[12:32]) or sum(w[:4096]) % WORD_MASK:
        raise DrumError('Directory reserved words or checksum are invalid.')
    count = 0
    for t in range(1,32):
        p = 32+(t-1)*8
        record = w[p:p+8]
        samples = w[t*4096:(t+1)*4096]
        if t > ntracks:
            if any(record) or any(samples):
                raise DrumError('Unused tracks and directory records must be zero.')
            continue
        track, first, valid, check, flags, r0, r1, r2 = record
        if track != t or first != count or not 1 <= valid <= 4096 or any((flags,r0,r1,r2)):
            raise DrumError(f'Invalid directory record for track {t}.')
        if t < ntracks and valid != 4096:
            raise DrumError('Only the last sample track may be partially filled.')
        if sum(samples) % WORD_MASK != check:
            raise DrumError(f'Checksum mismatch on track {t}.')
        if any(samples[valid:]) or WORD_MASK in samples:
            raise DrumError(f'Track {t} has noncanonical zero/padding.')
        count += valid
    if count != total or any(w[32+31*8:4096]):
        raise DrumError('Directory sample total or reserved area is invalid.')
    return {'format':'SignalAnalysis v1', 'sample_rate':4096, 'fft_length':256,
            'sample_tracks':ntracks, 'valid_samples':total, 'seconds':total/4096,
            'frame_slots':ntracks*16, 'signal_id':w[10], 'sha256':digest(data)}

def describe(data):
    words = unpack(data)
    if words[0] == MAGIC:
        return validate_signal(data)
    return {'format':'existing PDP-1 drum', 'bytes':len(data), 'sha256':digest(data)}

def image_from_samples(samples, signal_id):
    """Pack up to 31 seconds of prepared signed sample codes; zero-pad only the tail."""
    if not 1<=len(samples)<=31*4096 or any(not isinstance(v,int) or not -D<v<D for v in samples):
        raise DrumError('Expected 1-126976 signed fractional sample codes.')
    if not isinstance(signal_id,int) or not 0<=signal_id<=WORD_MASK:
        raise DrumError('Signal ID must fit in an unsigned 18-bit word.')
    ntracks=(len(samples)+4095)//4096
    encoded=[v if v>=0 else WORD_MASK^-v for v in samples]
    w=[0]*(TRACKS*WORDS_PER_TRACK)
    w[4096:4096+len(encoded)]=encoded
    for k,v in {0:MAGIC,1:1,2:4096,3:256,4:ntracks,5:len(samples),6:17,7:1,8:8,9:0,10:signal_id}.items():
        w[k]=v
    for track in range(1,ntracks+1):
        first=(track-1)*4096
        block=w[track*4096:(track+1)*4096]
        offset=32+(track-1)*8
        w[offset:offset+8]=[track,first,min(4096,len(samples)-first),sum(block)%WORD_MASK,0,0,0,0]
    w[11]=(-sum(w[:4096]))%WORD_MASK
    data=struct.pack(f'<{len(w)}I',*w)
    validate_signal(data)
    return data

def calibration_image():
    """One second of 512-CPS (1/2 peak) plus 1024-CPS (1/4 peak) calibration input."""
    samples=[round(D*(0.5*math.sin(math.tau*512*n/4096) + 0.25*math.sin(math.tau*1024*n/4096)))
             for n in range(4096)]
    return image_from_samples(samples,1)

def changing_plan():
    return [
        {'first_frame':0,'last_frame':3,'tones':[{'cps':256,'peak':0.5}]},
        {'first_frame':4,'last_frame':7,'tones':[{'cps':512,'peak':0.5}]},
        {'first_frame':8,'last_frame':11,'tones':[{'cps':512,'peak':0.5},{'cps':1024,'peak':0.25}]},
        {'first_frame':12,'last_frame':15,'tones':[{'cps':1024,'peak':0.25}]},
    ]

def changing_calibration_image():
    """Four four-frame scenes: moving peak, two tones, then a lower-power tone."""
    samples=[]
    for scene in changing_plan():
        for frame in range(scene['first_frame'],scene['last_frame']+1):
            for n in range(256):
                value=round(D*sum(t['peak']*math.sin(math.tau*t['cps']*n/4096) for t in scene['tones']))
                samples.append(value)
    return image_from_samples(samples,2)

def sync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)

def atomic_write(path, data, replace=True, mode=0o644):
    path = Path(path)
    fd, temp = tempfile.mkstemp(prefix='.'+path.name+'.', dir=path.parent)
    try:
        with os.fdopen(fd,'wb') as f:
            os.fchmod(f.fileno(),mode)
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        if replace:
            os.replace(temp,path)
        else:
            os.link(temp,path)  # Exclusive publication: never overwrite a named backup/image.
            os.unlink(temp)
        sync_dir(path.parent)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)

class Runtime:
    """Use the existing command port; never load/step/change CPU registers or debugger policy."""
    def __init__(self, root):
        self.root = Path(root).resolve()

    def pids(self):
        if not Path('/proc').is_dir():
            raise DrumError('Live switching is supported on the Linux Pi; use inspect/create elsewhere.')
        found = []
        for p in Path('/proc').iterdir():
            if not p.name.isdigit():
                continue
            try:
                if (p/'comm').read_text().strip() != 'pdp1':
                    continue
                if (p/'cwd').resolve(strict=True) == self.root:
                    found.append(int(p.name))
            except FileNotFoundError:
                continue
            except PermissionError as e:
                raise DrumError('Cannot inspect an emulator process; no image was switched.') from e
        if len(found)>1:
            raise DrumError('Multiple emulator processes use this directory.')
        return found

    def state(self):
        pids = self.pids()
        if not pids:
            return 'not running'
        try:
            with socket.create_connection(('127.0.0.1',1040),timeout=2) as s:
                s.sendall(b'run ?\n')
                reply=b''
                while b'\n' not in reply and len(reply)<1024:
                    b=s.recv(1024-len(reply))
                    if not b: break
                    reply+=b
        except OSError as e:
            raise DrumError('Cannot read the PDP-1 run state; no image was switched.') from e
        result=reply.decode('ascii',errors='replace').strip()
        if result not in ('running','halted','power off'):
            raise DrumError('Unrecognized run state: '+repr(result))
        return result

    def require_stopped(self):
        state = self.state()
        if state == 'running':
            raise DrumError('PDP-1 is running. Reach an idle point, halt it, then pick the image again.')
        return state

    def prepare_switch(self):
        """Reassert Stop after the CPU is halted so device close notifications stick.

        In the installed main loop, the final running cycle can call iotStart
        just after the first Stop notification. A second Stop with run already
        clear closes that handle without executing another CPU instruction.
        """
        state=self.require_stopped()
        if state!='halted':return
        try:
            with socket.create_connection(('127.0.0.1',1040),timeout=2) as s:
                s.sendall(b'halt\n')
                reply=s.recv(1024).decode('ascii',errors='replace').strip()
                if reply!='ok':raise DrumError('Stop notification was not acknowledged.')
        except OSError as e:
            raise DrumError('Could not prepare stopped devices for switching.') from e

    def wait_closed(self, active, timeout=5):
        """iotStop queues close after writes; fd disappearance establishes write-back completion."""
        deadline=time.monotonic()+timeout
        target=active.stat()
        identity=(target.st_dev,target.st_ino)
        while True:
            self.require_stopped()
            opened=False
            for pid in self.pids():
                try:
                    fds=list((Path('/proc')/str(pid)/'fd').iterdir())
                    for fd in fds:
                        try:
                            st=fd.stat()
                            if (st.st_dev,st.st_ino)==identity:
                                opened=True
                        except FileNotFoundError:
                            pass
                except FileNotFoundError:
                    continue
                except PermissionError as e:
                    raise DrumError('Cannot confirm drum write-back completion.') from e
            if not opened:
                return
            if time.monotonic()>=deadline:
                raise DrumError('Drum file is still open; pending writes may remain. No image was switched.')
            time.sleep(.05)

class Library:
    def __init__(self, root, runtime=None):
        self.root=Path(root).resolve()
        self.library=self.root/'Drums'
        self.home=self.root/'FunStuff'/'SignalAnalysis'
        self.active=self.root/'pdp23drum'
        self.journal=self.home/'drum-state.json'
        self.runtime=runtime if runtime is not None else Runtime(self.root)

    @contextlib.contextmanager
    def locked(self):
        self.home.mkdir(parents=True,exist_ok=True)
        with (self.home/'drum-switch.lock').open('a') as f:
            fcntl.flock(f,fcntl.LOCK_EX)
            yield

    def read_state(self):
        if not self.journal.exists():
            return {'version':1,'current':None,'history':[]}
        try:
            state=json.loads(self.journal.read_text())
            if state['version']!=1 or not isinstance(state['history'],list):
                raise ValueError()
            return state
        except (KeyError,ValueError,TypeError) as e:
            raise DrumError('Switch journal is invalid; keep it and use the named backup directly.') from e

    def write_state(self,state):
        atomic_write(self.journal,(json.dumps(state,indent=2)+'\n').encode())

    def create(self, path=None, changing=False):
        self.library.mkdir(parents=True,exist_ok=True)
        path=Path(path) if path else self.library/('SignalAnalysis-Changing.drm' if changing else 'SignalAnalysis.drm')
        data=changing_calibration_image() if changing else calibration_image()
        if path.exists():
            existing=path.read_bytes()
            info=validate_signal(existing)
            if existing!=data:
                raise DrumError('Image already exists with different content; preserving it: '+str(path))
            return {'created':False,'image':str(path),**info}
        atomic_write(path,data,replace=False)
        return {'created':True,'image':str(path),**validate_signal(data)}

    def pick(self,path,restore=False):
        with self.locked():
            return self._pick(Path(path).resolve(),restore=restore)

    def _pick(self,path,restore=False):
        # Read/validate the new image before touching any active or backup file.
        data=path.read_bytes()
        info=describe(data)
        self.runtime.require_stopped()
        if self.active.is_symlink() or not self.active.is_file():
            raise DrumError('Active drum must be an existing regular file, not a symlink.')
        self.runtime.prepare_switch()
        self.runtime.wait_closed(self.active)
        old=self.active.read_bytes()
        describe(old)
        if old==data:
            return {'changed':False,'image':str(path),'sha256':digest(data)}
        self.library.mkdir(parents=True,exist_ok=True)
        state=self.read_state()
        ident=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')+'-'+uuid.uuid4().hex[:8]
        backup=self.library/('Before-'+('Restore' if restore else 'SignalAnalysis')+'-'+ident+'.drm')
        mode=stat.S_IMODE(self.active.stat().st_mode)
        atomic_write(backup,old,replace=False)
        if backup.read_bytes()!=old:
            raise DrumError('Named backup verification failed; active drum was not replaced.')
        record={'id':ident,'action':'restore' if restore else 'pick','stage':'prepared',
                'image':str(path),'image_sha256':digest(data),'previous':str(backup),
                'previous_sha256':digest(old)}
        previous_current=state['current']
        state['history'].append(record)
        if not restore: state['current']=ident
        self.write_state(state)  # Recovery information is durable before replacing the active image.
        self.runtime.require_stopped()
        self.runtime.wait_closed(self.active)
        if self.active.read_bytes()!=old:
            raise DrumError('Active drum changed during preparation; it was not replaced. Named backup retained.')
        atomic_write(self.root/'pdp23drum.bak',old)
        self.runtime.require_stopped()
        if self.active.read_bytes()!=old:
            raise DrumError('Active drum changed before replacement; it was not replaced.')
        atomic_write(self.active,data,mode=mode)
        if self.active.read_bytes()!=data:
            raise DrumError('Active image changed after replacement. Named backup retained: '+str(backup))
        record['stage']='committed'
        if restore:
            record['restores']=previous_current
            state['current']=None
        self.write_state(state)
        return {'changed':True,'image':str(path),'backup':str(backup),'sha256':info['sha256'],
                'runtime':self.runtime.require_stopped(), 'left_stopped':True}

    def restore(self):
        with self.locked():
            state=self.read_state()
            ident=state['current']
            if ident is None:
                raise DrumError('No workstation selection to restore; use pick with a named image if needed.')
            record=next((r for r in state['history'] if r['id']==ident),None)
            if record is None:
                raise DrumError('Missing journal entry.')
            backup=Path(record['previous'])
            if digest(backup.read_bytes())!=record['previous_sha256']:
                raise DrumError('Named backup has changed; refusing to restore it.')
            if record['stage']=='prepared':
                current=digest(self.active.read_bytes())
                if current==record['previous_sha256']:
                    self.runtime.require_stopped()
                    record['stage']='aborted'
                    state['current']=None
                    self.write_state(state)
                    return {'changed':False,'message':'Interrupted selection had not replaced the active drum.'}
                if current!=record['image_sha256']:
                    raise DrumError('Interrupted switch has an unexpected active image; inspect named backups first.')
            result=self._pick(backup,restore=True)
            if not result['changed']:
                state['current']=None
                self.write_state(state)
            return result

    def status(self):
        state=self.read_state()
        result={'runtime':self.runtime.state(),'active':str(self.active),
                'selection':state['current'],'history_entries':len(state['history'])}
        if self.active.exists(): result['active_image']=describe(self.active.read_bytes())
        if state['current']:
            result['last_selection']=next(r for r in state['history'] if r['id']==state['current'])
        return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=DEFAULT_ROOT)
    sub=parser.add_subparsers(dest='command',required=True)
    sub.add_parser('create',help='Create the calibration image in Drums; never replace an existing image.')
    sub.add_parser('create-changing',help='Create the changing four-scene calibration image in Drums.')
    p=sub.add_parser('inspect',help='Validate a library or active image without changing it.')
    p.add_argument('image',type=Path)
    p=sub.add_parser('pick',help='Back up and select an image; requires a stopped PDP-1.')
    p.add_argument('image',nargs='?',type=Path)
    sub.add_parser('restore',help='Restore the image from before the last workstation selection.')
    sub.add_parser('status',help='Read the run state, active image, and switch journal.')
    args=parser.parse_args()
    lib=Library(args.root)
    try:
        if args.command=='create': result=lib.create()
        elif args.command=='create-changing': result={**lib.create(changing=True),'scenes':changing_plan()}
        elif args.command=='inspect': result=describe(args.image.read_bytes())
        elif args.command=='pick': result=lib.pick(args.image or lib.library/'SignalAnalysis.drm')
        elif args.command=='restore': result=lib.restore()
        else: result=lib.status()
        print(json.dumps(result,indent=2))
    except (DrumError,OSError) as e:
        print('drum: '+str(e),file=sys.stderr)
        return 1
    return 0

if __name__=='__main__':
    raise SystemExit(main())
