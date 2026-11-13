#!/usr/bin/env python3
"""Prepare a WAV recording for the PiDP-1 workstation; never select/load it automatically."""
import argparse
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from drum import DEFAULT_ROOT, D, DrumError, atomic_write, digest, image_from_samples, validate_signal

RESAMPLER='aresample=4096:resampler=swr:filter_size=64:phase_shift=10:cutoff=0.90:exact_rational=1:dither_method=0:osf=dbl:tsf=dblp'
CODECS={'pcm_u8','pcm_s16le','pcm_s16be','pcm_s24le','pcm_s24be',
        'pcm_s32le','pcm_s32be','pcm_f32le','pcm_f32be','pcm_f64le','pcm_f64be'}

def execute(args, timeout=60):
    try:
        result=subprocess.run(args,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                              timeout=timeout,check=False)
    except (OSError,subprocess.TimeoutExpired) as e:
        raise DrumError('Audio tool could not finish: '+str(e)) from e
    if result.returncode:
        raise DrumError('Audio conversion failed: '+result.stderr.decode(errors='replace')[-2000:])
    return result.stdout

def probe(path):
    tool=shutil.which('ffprobe')
    if not tool:raise DrumError('ffprobe is required; install FFmpeg.')
    data=json.loads(execute([tool,'-v','error','-show_streams','-show_format','-of','json',str(path)]))
    audio=[s for s in data.get('streams',[]) if s.get('codec_type')=='audio']
    if data.get('format',{}).get('format_name')!='wav' or len(audio)!=1:
        raise DrumError('Input must be a WAV with one uncompressed audio stream.')
    stream=audio[0]
    if stream.get('codec_name') not in CODECS:
        raise DrumError('Supported WAV formats are PCM 8/16/24/32-bit and IEEE float 32/64-bit.')
    try:
        rate=int(stream['sample_rate']);channels=int(stream['channels'])
        seconds=float(stream.get('duration') or data.get('format',{}).get('duration'))
    except (KeyError,ValueError,TypeError) as e:
        raise DrumError('Cannot determine WAV duration/rate/channels.') from e
    if not rate>0 or not 1<=channels<=32 or not math.isfinite(seconds) or seconds<=0:
        raise DrumError('Invalid WAV duration/rate/channels.')
    return {'sample_rate':rate,'channels':channels,'seconds':seconds,'codec':stream['codec_name']}

def prepare(path, start=0., duration=None, channel='mix', gain=1.):
    path=Path(path).resolve(strict=True)
    if not path.is_file():raise DrumError('Input must be a local WAV file.')
    info=probe(path)
    if not math.isfinite(start) or not 0<=start<info['seconds']:
        raise DrumError('Start must be within the recording.')
    available=info['seconds']-start
    if duration is None:
        if available>31:raise DrumError('Recording exceeds 31 seconds; choose --start and --duration (at most 31).')
        duration=available
    if not math.isfinite(duration) or not 0<duration<=31:
        raise DrumError('Duration must be greater than zero and at most 31 seconds.')
    duration=min(duration,available)
    if not math.isfinite(gain) or gain<0:raise DrumError('Gain must be finite and nonnegative.')
    channels=info['channels']
    if channel=='mix':
        terms='+'.join(f'{1/channels:.17g}*c{n}' for n in range(channels))
        channel_description='equal-weight average of all channels'
    else:
        try:index={'left':0,'right':1}.get(channel,int(channel)-1 if channel.isdecimal() else -1)
        except ValueError:index=-1
        if not 0<=index<channels:raise DrumError('Requested channel is not in this WAV (channel numbers begin at 1).')
        terms=f'c{index}';channel_description=f'channel {index+1}'
    tool=shutil.which('ffmpeg')
    if not tool:raise DrumError('ffmpeg is required; install FFmpeg.')
    filters=f'aformat=sample_fmts=dbl,pan=mono|c0={terms},{RESAMPLER}'
    command=[tool,'-nostdin','-hide_banner','-v','error','-xerror','-ss',f'{start:.12g}',
             '-i',str(path),'-map','0:a:0','-vn','-sn','-dn','-af',filters,
             '-t',f'{duration:.12g}','-ar','4096','-c:a','pcm_f64le','-f','f64le','pipe:1']
    raw=execute(command)
    if len(raw)%8 or not raw:raise DrumError('Conversion produced no complete samples.')
    count=len(raw)//8
    if count>31*4096:raise DrumError('Resampled recording exceeds drum capacity.')
    decoded=struct.unpack(f'<{count}d',raw)
    codes=[];clamped=0;input_peak=0.
    for value in decoded:
        value*=gain
        if not math.isfinite(value):raise DrumError('Recording/gain produced a non-finite sample.')
        input_peak=max(input_peak,abs(value))
        if value>=1 or value<=-1:
            code=D-1 if value>0 else -(D-1);clamped+=1
        else:
            code=round(value*D)
            if not -D<code<D:
                code=max(-(D-1),min(D-1,code));clamped+=1
        codes.append(code)
    version=execute([tool,'-version']).decode(errors='replace').splitlines()[0]
    return codes,{'input':str(path),'input_sha256':digest(path.read_bytes()),'source':info,
                  'start_seconds':start,'requested_seconds':duration,'channel_selection':channel_description,
                  'gain':gain,'automatic_normalization':False,'resampler':RESAMPLER,
                  'ffmpeg_version':version,'clamped_samples':clamped,'peak_after_gain':input_peak,
                  'converted_samples':count,'converted_seconds':count/4096}

def import_wav(source, output, root=DEFAULT_ROOT, start=0., duration=None, channel='mix', gain=1.,
               credit=None, source_url=None, output_format="drum"):
    output=Path(output).resolve()
    root=Path(root).resolve()
    if output in (root/'pdp23drum',root/'pdp23drum.bak'):
        raise DrumError('Import to the Drums library; use drum.py pick to select an image.')
    provenance=output.with_suffix(output.suffix+'.json')
    if output.exists() or provenance.exists():raise DrumError('Output or provenance already exists; choose a new name.')
    codes,report=prepare(source,start,duration,channel,gain)
    if output_format=='tape':
        from tape import from_samples,validate_recording
        data=from_samples(codes,3);report.update(validate_recording(data))
    else:
        data=image_from_samples(codes,3);report.update(validate_signal(data))
    report.update({'sha256':digest(data),'output':str(output),'padded_samples':report['sample_tracks']*4096-len(codes),
                   'credit':credit,'source_url':source_url,'host_processing':'decode, channel mix, anti-alias/resample, explicit gain, quantize only; no FFT'})
    output.parent.mkdir(parents=True,exist_ok=True)
    # Publish provenance first, then the independently validated image. Neither overwrites.
    metadata=(json.dumps(report,indent=2)+'\n').encode()
    atomic_write(provenance,metadata,replace=False)
    try:
        atomic_write(output,data,replace=False)
    except OSError:
        if provenance.read_bytes()==metadata:
            provenance.unlink()
        raise
    return report

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('wav',type=Path)
    p.add_argument('--root',type=Path,default=DEFAULT_ROOT)
    p.add_argument('--output',type=Path)
    p.add_argument('--format',choices=['drum','tape'],default='drum',help='drum image or Microtape recording words (11 seconds maximum)')
    p.add_argument('--start',type=float,default=0.)
    p.add_argument('--duration',type=float)
    p.add_argument('--channel',default='mix',help='mix (default), left, right, or a channel number starting at 1')
    p.add_argument('--gain',type=float,default=1.,help='explicit amplitude multiplier; default 1, no automatic normalization')
    p.add_argument('--credit')
    p.add_argument('--source-url')
    a=p.parse_args()
    output=a.output or (a.root/'FunStuff/SignalAnalysis/samples'/(a.wav.stem+'.words') if a.format=='tape' else a.root/'Drums'/(a.wav.stem+'.drm'))
    try:
        report=import_wav(a.wav,output,a.root,a.start,a.duration,a.channel,a.gain,a.credit,a.source_url,a.format)
        print(json.dumps(report,indent=2))
    except (OSError,DrumError,ValueError) as e:
        p.exit(1,'import_wav: '+str(e)+'\n')

if __name__=='__main__':main()
