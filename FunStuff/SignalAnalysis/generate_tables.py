#!/usr/bin/env python3
"""Generate fixed mathematical constants only; no runtime signal analysis."""
import math
import json
from pathlib import Path
D = 1 << 17
MASK = (1 << 18) - 1
ROOT = Path(__file__).resolve().parent

def encode(n):
    return n if n >= 0 else MASK ^ -n

def generate():
    dest = ROOT / 'generated'
    dest.mkdir(exist_ok=True)
    cos = [math.trunc(D * math.cos(math.tau * k / 256)) for k in range(128)]
    sin = [math.trunc(-D * math.sin(math.tau * k / 256)) for k in range(128)]
    cos[0], sin[0], cos[64], sin[64] = D-1, 0, 0, -(D-1)
    assert all(c*c+s*s <= D*D for c,s in zip(cos,sin))
    text = '// Inward-truncated 17-fraction-bit coefficients; cardinal cases bypass MUL.\n'
    for label, data in [('cosine',cos),('nsine',sin)]:
        text += label + ',\n' + ''.join(f'        {encode(v):06o}\n' for v in data)
    (dest / 'twiddles.ac').write_text(text)
    rev = [int(f'{n:08b}'[::-1],2) for n in range(256)]
    (dest / 'bitrev.ac').write_text(''.join(f'        {n:06o}\n' for n in rev))
    lines=['// Static Type 340 commands; no runtime signal processing on the host.', 'scene,']
    def put(word): lines.append('        '+word)
    def label(x,y,text,size=1):
        put(f'scale({size}) intensity(5) lpoff next(point)')
        put(f'y(0d{y}) next(point)')
        put(f'x(0d{x}) next(character)')
        put('type340 "'+text+'"')
    def loading_pause():
        # Unlit X-positioning words cost 35 us each on the Type 340. Pace the
        # autonomous loading/error lists to roughly 10+ ms, rather than redrawing
        # bright text at ~1 kHz and competing unnecessarily with tape polling.
        put('scale(0) lpoff next(point)')
        for _ in range(255):put('x(0d128) next(point)')
        put('x(0d128) next(parameter)')
    def horizontal(y):
        put('scale(2) intensity(2) lpoff next(point)')
        put(f'y(0d{y}) next(point)')
        put('x(0d128) next(vector)')
        # Spacing 4: 768 pixels = 4*(63+63+63+3). Short BRM counts
        # are faster than unit-spaced axes; the faint baseline becomes dotted.
        for _ in range(3): put('visible deltax(0d63)')
        put('visible deltax(0d3) end')
    label(128,980,'PDP-1 SIGNAL ANALYZER',1)
    label(128,930,'SIGNAL / 4096 SAMPLES PER SECOND')
    label(128,390,'S1 RUN   S2 NEXT   S3 GAIN')
    horizontal(768);horizontal(64)
    for k,f in [(0,0),(32,512),(64,1024),(96,1536),(128,2048)]:
        put('scale(0) intensity(2) next(point)')
        put('y(0d64) next(point)')
        put(f'x(0d{128+6*k}) next(vector)')
        put('visible deltay(0d8) end')
        label(max(128,128+6*k-12),28,str(f))
    label(960,28,'CPS')
    cfg=json.loads((ROOT/'generated/tape-config.json').read_text())
    lines += ['sceneend,','source_drum,']
    label(680,930,'DRUM')
    lines += ['source_drum_end,','source_tape,']
    label(680,930,f'TAPE {cfg["drive"]} {cfg["name"]}')
    lines += ['source_tape_end,','loading_scene,']
    label(128,600,f'LOADING {cfg["name"]}')
    label(128,550,f'MICROTAPE DRIVE {cfg["drive"]}')
    loading_pause()
    put('next(subroutine)')
    put('jump loading_scene:1 next(parameter)')
    lines += ['loading_error,']
    label(128,600,'TAPE LOAD FAILED')
    label(128,550,'CHECK TAPE AND FILE NAME')
    loading_pause()
    put('next(subroutine)')
    put('jump loading_error:1 next(parameter)')
    lines += ['gain4,']
    label(128,440,'ONE-SIDED POWER X4',1)
    lines += ['gain4end,','gain64,']
    label(128,440,'ONE-SIDED POWER X64',1)
    lines += ['gain64end,','footer,']
    put('scale(1) intensity(5) lpoff next(point)')
    put('y(0d440) next(point)')
    put('x(0d600) next(character)')
    put('type340 "TRACK "')
    lines += ['footerend,','framelabel,']
    put('type340 " FRAME "')
    lines += ['framelabelend,','tracknums,']
    for n in range(32):put(f'type340 "{n:02d}"')
    lines += ['tracknumsend,','assert tracknumsend-tracknums == 040, "track number encoding size"','framenums,']
    for n in range(16):put(f'type340 "{n:02d}"')
    lines += ['framenumsend,','assert framenumsend-framenums == 020, "frame number encoding size"']
    (dest/'scene.ac').write_text('\n'.join(lines)+'\n')


if __name__ == '__main__':
    generate()
