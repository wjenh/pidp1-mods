"""Nominal Type 340/342 scan cost, using the installed emulator's timing model.

Counts 5 us channel fetches, X positioning, 3..7-bit shortened BRM vectors,
and the Type 342 character-path table. Excludes host scheduling, HSC contention,
and sequence-break latency; estimates must be compared with measured timings.
"""
CHAR_STEPS=[19,22,24,20,22,26,20,22,24,17,16,20,19,20,21,20,
            20,23,21,20,19,22,21,24,20,18,26,12,9,0,0,0,
            7,14,14,25,23,23,20,13,15,13,19,14,11,10,9,10,
            23,15,22,19,18,22,22,17,22,20,14,16,15,16,13,17]


def cost(words):
    mode=0;scale=0;x=y=0;totals={'fetch':0,'position':0,'waveform':0,'spectrum':0,'other_vectors':0,'characters':0}
    for index,w in enumerate(words):
        totals['fetch']+=5000
        nxt=(w>>13)&7
        if mode==0:
            if w&0o2000:break
            if w&0o100:scale=(w>>4)&3
            mode=nxt
        elif mode==1:
            if w&0o200000:
                y=w&0o1777
                if w&0o2000:totals['position']+=35000
            else:x=w&0o1777;totals['position']+=35000
            mode=nxt
        elif mode==3:
            for shift in (12,6,0):
                c=(w>>shift)&63
                totals['characters']+=CHAR_STEPS[c]*1500+500
                if c==0o37:mode=0;break
        elif mode==4:
            dx=w&127;dy=(w>>8)&127
            pulses=(1<<max(3,max(dx,dy).bit_length()))-1
            category='waveform' if min(y,y+dy*(1<<scale)*(-1 if w&0o100000 else 1))>500 else 'spectrum' if dx==0 and w&0o200000 and y>=64 else 'other_vectors'
            totals[category]+=500+pulses*1500
            x+=dx*(1<<scale)*(-1 if w&0o200 else 1)
            y+=dy*(1<<scale)*(-1 if w&0o100000 else 1)
            if w&0o400000:mode=0
        else:raise ValueError(f'Unsupported mode {mode}')
    else:raise ValueError('No list HALT')
    return {'words':index+1,'nominal_ms':sum(totals.values())/1e6,'parts_ms':{k:v/1e6 for k,v in totals.items()}}


if __name__=='__main__':
    import json,sys
    rows=[cost([int(v,8) for v in line.split()[1:]]) for line in open(sys.argv[1])]
    for n,row in enumerate(rows):row['frame']=n%144
    print(json.dumps({'frames':len(rows),'max':max(rows,key=lambda r:r['nominal_ms']),
                      'min':min(rows,key=lambda r:r['nominal_ms']),
                      'mean_ms':sum(r['nominal_ms'] for r in rows)/len(rows),
                      'over_25ms':sum(r['nominal_ms']>25 for r in rows),'all':rows},indent=2))
