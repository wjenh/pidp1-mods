#!/usr/bin/env python3
import sys,unittest,tempfile
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from tape import from_samples,validate_recording,config,select_drive,DrumError


class TapeFormatTests(unittest.TestCase):
    def test_roundtrip_and_padding(self):
        info=validate_recording(from_samples([1,-1,131071,-131071,0]))
        self.assertEqual(info['valid_samples'],5);self.assertEqual(info['frame_slots'],16)
    def test_maximum(self):
        self.assertEqual(validate_recording(from_samples([0]*(11*4096)))['sample_tracks'],11)
        with self.assertRaises(DrumError):from_samples([0]*(11*4096+1))
    def test_corruption(self):
        data=bytearray(from_samples([1]*256))
        for offset in (0,48,1024,len(data)-4):
            bad=data.copy();bad[offset]^=1
            with self.assertRaises(DrumError):validate_recording(bad)
    def test_config(self):
        self.assertEqual(config(name='apollo11')['name'],'APOLLO11')
        for kwargs in ({'name':'ABCDEFGHIJ'},{'drive':0},{'drive':9},{'image':'x,locked'},{'image':'"bad"'}):
            with self.assertRaises(DrumError):config(**kwargs)
    def test_preserve_other_drives(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);image=root/'samples.img';image.write_bytes(b'example')
            listing=root/'microtapes.txt';before='2 /tapes/saves.img\n1 /tapes/eliza.mtp\n';listing.write_text(before)
            with patch('tape.Runtime') as runtime:
                select_drive(root,image,8)
                runtime.return_value.require_stopped.assert_called_once()
            self.assertTrue(listing.read_text().startswith(before))
            self.assertIn(',locked',listing.read_text())
            backup=list((root/'FunStuff/SignalAnalysis/snapshots').glob('microtapes-*'))
            self.assertEqual(len(backup),1);self.assertEqual(backup[0].read_text(),before)
            with patch('tape.Runtime'):select_drive(root,image,8)
            self.assertEqual(len(list(backup[0].parent.glob('microtapes-*'))),1)
    def test_refuse_assigned_drive(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);image=root/'samples.img';image.write_bytes(b'example')
            listing=root/'microtapes.txt';before='8 /other/tape.img\n';listing.write_text(before)
            with patch('tape.Runtime'),self.assertRaises(DrumError):select_drive(root,image,8)
            self.assertEqual(listing.read_text(),before)


if __name__=='__main__':unittest.main()
