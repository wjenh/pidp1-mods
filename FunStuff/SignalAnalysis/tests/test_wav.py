#!/usr/bin/env python3
"""Decode, amplitude, channel selection, anti-aliasing, capacity and publication checks."""
import math
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
import wave
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from drum import DrumError,D,digest,calibration_image,changing_calibration_image,unpack,validate_signal,image_from_samples
from import_wav import prepare,import_wav,execute

@unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'),'FFmpeg tools required')
class WavTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
    def tearDown(self):self.tmp.cleanup()
    def wav(self,name,values,rate=4096,channels=1):
        p=self.root/name
        with wave.open(str(p),'wb') as f:
            f.setnchannels(channels);f.setsampwidth(2);f.setframerate(rate)
            f.writeframes(struct.pack(f'<{len(values)}h',*values))
        return p
    def test_pcm_mapping_and_partial_track(self):
        p=self.wav('exact.wav',[0,1,-1,16384,-16384,32767,-32768]*100)
        codes,meta=prepare(p)
        self.assertEqual(codes[:7],[0,4,-4,65536,-65536,131068,-131071])
        self.assertEqual(meta['clamped_samples'],100)
        out=self.root/'exact.drm';report=import_wav(p,out,root=self.root)
        self.assertEqual(report['valid_samples'],700)
        w=unpack(out.read_bytes());self.assertTrue(all(x==0 for x in w[4096+700:8192]))
        self.assertTrue(out.with_suffix('.drm.json').is_file())
    def test_24bit_precision_and_nonfinite_float(self):
        p=self.root/'precise.wav'
        values=[64,-64,1,-1]*1024
        with wave.open(str(p),'wb') as f:
            f.setnchannels(1);f.setsampwidth(3);f.setframerate(4096)
            f.writeframes(b''.join(v.to_bytes(3,'little',signed=True) for v in values))
        codes,_=prepare(p)
        self.assertEqual(codes[:4],[1,-1,0,0])
        p=self.root/'nan.wav'
        data=struct.pack('<1024f',*[float('nan')]*1024)
        fmt=struct.pack('<HHIIHH',3,1,4096,4096*4,4,32)
        p.write_bytes(b'RIFF'+struct.pack('<I',4+8+len(fmt)+8+len(data))+b'WAVEfmt '+struct.pack('<I',len(fmt))+fmt+b'data'+struct.pack('<I',len(data))+data)
        with self.assertRaises(DrumError):prepare(p)

    def test_mix_and_channel_selection(self):
        p=self.wav('stereo.wav',[8192,-8192]*4096,channels=2)
        mixed,_=prepare(p);left,_=prepare(p,channel='left');right,_=prepare(p,channel='2')
        self.assertTrue(all(v==0 for v in mixed))
        self.assertTrue(all(v==32768 for v in left))
        self.assertTrue(all(v==-32768 for v in right))
        with self.assertRaises(DrumError):prepare(p,channel='3')
    def test_gain_no_auto_normalization(self):
        p=self.wav('gain.wav',[8192]*4096)
        base,m=prepare(p);boost,mb=prepare(p,gain=8)
        self.assertEqual(base,[32768]*4096)
        self.assertFalse(m['automatic_normalization'])
        self.assertEqual(boost,[131071]*4096)
        self.assertEqual(mb['clamped_samples'],4096)
    def test_antialias_and_passband(self):
        def tone(f):return self.wav(f'{f}.wav',[round(16384*math.sin(math.tau*f*n/16384)) for n in range(16384)],16384)
        passed,_=prepare(tone(512));stopped,_=prepare(tone(3000))
        rms=lambda a:math.sqrt(sum(v*v for v in a[256:-256])/len(a[256:-256]))
        self.assertAlmostEqual(rms(passed)/(D*.5/math.sqrt(2)),1,delta=.005)
        self.assertLess(rms(stopped),rms(passed)/1000)
    def test_start_and_duration(self):
        p=self.wav('cut.wav',[8192]*2048+[-8192]*2048)
        codes,m=prepare(p,start=.5,duration=.25)
        self.assertEqual(codes,[-32768]*1024)
        self.assertEqual(m['start_seconds'],.5)
    def test_supported_encodings(self):
        original=self.wav('source.wav',[8192,-8192]*2048)
        for codec in ('pcm_u8','pcm_s24le','pcm_s32le','pcm_f32le','pcm_f64le'):
            p=self.root/(codec+'.wav')
            execute([shutil.which('ffmpeg'),'-v','error','-i',str(original),'-c:a',codec,str(p)])
            codes,m=prepare(p)
            self.assertEqual(codes[:2],[32768,-32768],codec)
            self.assertEqual(len(codes),4096)
    def test_capacity_and_multi_track_directory(self):
        p=self.wav('long.wav',[0]*(32*4096))
        with self.assertRaises(DrumError):prepare(p)
        codes,_=prepare(p,start=1,duration=31)
        info=validate_signal(image_from_samples(codes,3))
        self.assertEqual(info['sample_tracks'],31)
        self.assertEqual(info['valid_samples'],126976)
        info=validate_signal(image_from_samples([1]*5000,3))
        self.assertEqual(info['sample_tracks'],2)
        self.assertEqual(info['valid_samples'],5000)
    def test_bad_parameters_and_empty_input(self):
        p=self.wav('valid.wav',[0]*4096)
        for opts in ({'gain':float('nan')},{'start':-1},{'duration':0},{'duration':32},{'channel':'bad'}):
            with self.assertRaises(DrumError):prepare(p,**opts)
        bad=self.root/'bad.wav';bad.write_bytes(b'not a wav')
        with self.assertRaises(DrumError):prepare(bad)
    def test_output_protection_and_active_path(self):
        p=self.wav('valid.wav',[0]*4096);out=self.root/'keep.drm';out.write_bytes(b'keep')
        with self.assertRaises(DrumError):import_wav(p,out,root=self.root)
        self.assertEqual(out.read_bytes(),b'keep')
        with self.assertRaises(DrumError):import_wav(p,self.root/'pdp23drum',root=self.root)
    def test_calibration_images_unchanged(self):
        self.assertEqual(digest(calibration_image()),'c19afccc6af715663729459a1b9554e404a7fd6d6fb087ab826c19cbfbccfe93')
        self.assertEqual(digest(changing_calibration_image()),'e5397d59128f76e190acbb41e2f0804cf395365ddf72dd92af1a3af8d6aebffa')

if __name__=='__main__':unittest.main(verbosity=2)
