#!/usr/bin/env python3
"""Check all actual generated Apollo scenes: endpoints, bounds, power bars and cost."""
import json
import struct
import sys
from pathlib import Path
from test_core import decode,trunc,reference
from test_frontend import scene
from display_cost import cost


def main(trace,image,before):
    raw=struct.unpack('<131072I',Path(image).read_bytes());rows=[]
    for frame,line in enumerate(Path(trace).read_text().splitlines()):
        words=[int(v,8) for v in line.split()];addr=words.pop(0)
        mem={addr+k:v for k,v in enumerate(words)}
        render=scene(mem,addr)
        vertices={tuple(p) for p in render['points']}
        for v in render['lines']:vertices.add(tuple(v[:2]));vertices.add(tuple(v[2:]))
        offset=4096+(frame%144)*256
        samples=[decode(v) for v in raw[offset:offset+256]]
        for n,sample in enumerate(samples):
            half=trunc(sample,2)
            expected=(128+3*n,768+trunc(half+trunc(half,2),512))
            assert expected in vertices,(frame,n,expected)
        for k,(r,i) in enumerate(reference(samples)[:129]):
            h=min(256,(r*r+i*i)>>17>>(1 if k in (0,128) else 0))
            if not h:continue
            x=128+6*k;y=64
            while y<64+h:
                ends=[v[3] for v in render['lines'] if v[0]==v[2]==x and v[1]==y and y<v[3]<=64+h]
                assert ends,(frame,k,h,y)
                y=max(ends)
        rows.append(cost(words))
    assert len(rows)==146
    old=json.loads(Path(before).read_text())
    result={'checked_scenes':len(rows),'sample_endpoints_checked':len(rows)*256,
            'all_bars_and_coordinates_checked':True,'max_list_words':max(r['words'] for r in rows),
            'nominal_before_max_ms':old['max']['nominal_ms'],
            'nominal_after_max_ms':max(r['nominal_ms'] for r in rows),
            'nominal_before_mean_ms':old['mean_ms'],
            'nominal_after_mean_ms':sum(r['nominal_ms'] for r in rows)/len(rows),
            'nominal_after_over_25ms':sum(r['nominal_ms']>25 for r in rows)}
    assert result['nominal_after_over_25ms']==0
    print(json.dumps(result,indent=2))


if __name__=='__main__':main(*sys.argv[1:])
