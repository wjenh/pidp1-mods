#!/usr/bin/env python3
"""Refresh the reviewed checkout integrity manifest before committing a release."""
import hashlib
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parent
result=subprocess.run(['git','ls-files','-z'],cwd=root,check=True,capture_output=True)
paths=sorted(Path(p.decode()) for p in result.stdout.split(b'\0') if p)
lines=[]
for relative in paths:
    if str(relative)=='SHA256SUMS':continue
    path=root/relative
    if path.is_file():lines.append(hashlib.sha256(path.read_bytes()).hexdigest()+'  '+relative.as_posix())
(root/'SHA256SUMS').write_text('\n'.join(lines)+'\n')
print(f'Updated integrity hashes for {len(lines)} tracked files')
