#!/usr/bin/env python3
"""Check distributed files and sample images without accessing live devices."""
import hashlib
import os
import subprocess
import tempfile
from pathlib import Path
from drum import validate_signal


def main():
    root=Path(__file__).resolve().parent
    entries=(root/'SHA256SUMS').read_text().splitlines()
    for line in entries:
        expected,name=line.split('  ',1)
        path=Path(name)
        if path.is_absolute() or '..' in path.parts:
            raise ValueError('Invalid manifest path: '+name)
        actual=hashlib.sha256((root/path).read_bytes()).hexdigest()
        if actual!=expected:raise ValueError('File differs from distributed version: '+name)
    images=sorted((root/'samples').glob('*.drm'))
    if len(images)!=3:raise ValueError('Expected three supplied drum images')
    for path in images:
        info=validate_signal(path.read_bytes())
        print(f"PASS {path.name}: {info['sample_tracks']} track(s), {info['valid_samples']} samples")
    tool=Path(os.environ.get('PDP1_ROOT','/opt/pidp1-mods'))/'bin/mtfs'
    if tool.is_file():
        from tape import validate_recording,from_drum
        with tempfile.TemporaryDirectory(prefix='saw-package-check-') as td:
            for name,source in [('APOLLO11','Apollo11.drm'),('TWOTONE','SignalAnalysis.drm'),('CHANGING','SignalAnalysis-Changing.drm')]:
                out=Path(td)/(name+'.words')
                result=subprocess.run([str(tool),'-x','-n',name,'-f','words','-o',str(out),str(root/'samples/SignalStation.mtp')],capture_output=True,text=True)
                if result.returncode:raise ValueError('Tape extraction failed: '+result.stderr)
                data=out.read_bytes();validate_recording(data)
                if data!=from_drum((root/'samples'/source).read_bytes()):raise ValueError('Tape samples differ from supplied source: '+name)
                print('PASS tape recording '+name)
    else:print('Tape image hash checked; content extraction skipped because mtfs is unavailable here')
    print(f'PASS {len(entries)} distributed files; no live devices accessed')


if __name__=='__main__':main()
