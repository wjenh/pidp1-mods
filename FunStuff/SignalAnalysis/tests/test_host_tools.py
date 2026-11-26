#!/usr/bin/env python3
"""Tool selection, installation isolation, overrides, and failure behavior."""
import os,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from tape import filesystem_tool,make_image,selected_recording,DrumError,write_config


class HostToolTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name).resolve();(self.root/'bin').mkdir()
    def executable(self,name):
        p=self.root/'bin'/name;p.write_text('#!/bin/sh\nexit 1\n');p.chmod(0o755);return p
    def test_prefer_fstool(self):
        self.executable('mtfs');new=self.executable('fstool')
        self.assertEqual(filesystem_tool(self.root),new)
    def test_legacy_fallback(self):
        old=self.executable('mtfs');self.assertEqual(filesystem_tool(self.root),old)
    def test_unusable_fstool(self):
        new=self.executable('fstool');new.chmod(0o644);old=self.executable('mtfs')
        self.assertEqual(filesystem_tool(self.root),old)
    def test_explicit_override(self):
        old=self.executable('mtfs');self.executable('fstool')
        self.assertEqual(filesystem_tool(self.root,old),old)
        with patch.dict(os.environ,PATH=str(self.root/'bin')):
            self.assertEqual(filesystem_tool(self.root,'fstool'),self.root/'bin/fstool')
    def test_bad_override_is_not_silently_replaced(self):
        self.executable('fstool')
        with self.assertRaises(DrumError):filesystem_tool(self.root,self.root/'absent')
    def test_no_tool_and_no_wrong_installation_from_PATH(self):
        other=self.root/'other';other.mkdir();fake=other/'fstool';fake.write_text('');fake.chmod(0o755)
        with patch.dict(os.environ,PATH=str(other)):
            self.assertIsNone(filesystem_tool(self.root,required=False))
            with self.assertRaises(DrumError):filesystem_tool(self.root)
    def test_pack_missing_tool_does_not_publish_an_image(self):
        out=self.root/'output.mtp'
        with self.assertRaises(DrumError):make_image(out,[('TEST',self.root/'absent.drm')],root=self.root)
        self.assertFalse(out.exists())
    def test_failed_fstool_is_not_retried_with_mtfs(self):
        self.executable('fstool');self.executable('mtfs')
        write_config(self.root,image='/some/tape.mtp')
        image=self.root/'some.mtp';image.write_bytes(b'example')
        write_config(self.root,image=str(image))
        from subprocess import CompletedProcess
        with patch('tape.subprocess.run',return_value=CompletedProcess([],1,'','BADBLOCK')) as run:
            with self.assertRaisesRegex(DrumError,'BADBLOCK'):selected_recording(self.root,self.root)
            self.assertEqual(run.call_count,1)
            self.assertEqual(run.call_args.args[0][0],str(self.root/'bin/fstool'))

if __name__=='__main__':unittest.main()
