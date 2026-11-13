#!/usr/bin/env python3
"""Image and switch transactions tested entirely in disposable directories."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import drum

class FakeRuntime:
    def __init__(self,state='halted'):
        self.current=state
        self.waits=0
        self.open=False
    def state(self): return self.current
    def require_stopped(self):
        if self.current=='running': raise drum.DrumError('running')
        return self.current
    def prepare_switch(self):self.require_stopped()
    def wait_closed(self,path):
        self.waits+=1
        if self.open: raise drum.DrumError('write queue not drained')

class DrumTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.root=Path(self.temp.name)
        (self.root/'Drums').mkdir()
        self.old=struct.pack('<I',0o123456)*(32*4096)
        (self.root/'pdp23drum').write_bytes(self.old)
        self.rt=FakeRuntime()
        self.lib=drum.Library(self.root,self.rt)
        self.lib.create()
        self.image=self.root/'Drums'/'SignalAnalysis.drm'
        self.new=self.image.read_bytes()
    def tearDown(self): self.temp.cleanup()
    def test_calibration_contract(self):
        info=drum.validate_signal(self.new)
        self.assertEqual(info['valid_samples'],4096)
        self.assertEqual(info['frame_slots'],16)
        w=drum.unpack(self.new)
        dec=lambda v:v if v<drum.D else -(drum.WORD_MASK^v)
        self.assertEqual([dec(v) for v in w[4096:4104]],[0,79109,65536,13573,0,-13573,-65536,-79109])
        self.assertEqual(w[4096:4104],w[8184:8192])
        self.assertEqual(sum(w[:4096])%drum.WORD_MASK,0)
    def test_changing_calibration(self):
        info=self.lib.create(changing=True)
        path=Path(info['image'])
        self.assertEqual(info['signal_id'],2)
        self.assertEqual(info['frame_slots'],16)
        self.assertNotEqual(path,self.image)
        self.assertEqual(self.image.read_bytes(),self.new)
        w=drum.unpack(path.read_bytes())
        frames=[w[4096+n*256:4096+(n+1)*256] for n in range(16)]
        for first in (0,4,8,12):
            self.assertTrue(all(frames[first]==frames[n] for n in range(first,first+4)))
        self.assertEqual(len({tuple(frames[n]) for n in (0,4,8,12)}),4)
        self.assertFalse(self.lib.create(changing=True)['created'])

    def test_create_does_not_overwrite(self):
        result=self.lib.create()
        self.assertFalse(result['created'])
        self.assertEqual(self.image.read_bytes(),self.new)
    def test_corrupt_image_refused(self):
        data=bytearray(self.new);data[4096*4+30]^=1
        self.image.write_bytes(data)
        with self.assertRaises(drum.DrumError):self.lib.pick(self.image)
        self.assertEqual(self.lib.active.read_bytes(),self.old)
        self.assertFalse((self.root/'pdp23drum.bak').exists())
    def test_running_refused_without_backup_or_journal(self):
        self.rt.current='running'
        with self.assertRaises(drum.DrumError):self.lib.pick(self.image)
        self.assertEqual(self.lib.active.read_bytes(),self.old)
        self.assertFalse(self.lib.journal.exists())
    def test_pending_close_refused(self):
        self.rt.open=True
        with self.assertRaises(drum.DrumError):self.lib.pick(self.image)
        self.assertEqual(self.lib.active.read_bytes(),self.old)
        self.assertFalse(self.lib.journal.exists())
    def test_switch_and_restore_exact_bytes(self):
        before_mode=self.lib.active.stat().st_mode&0o777
        result=self.lib.pick(self.image)
        self.assertTrue(result['left_stopped'])
        self.assertEqual(self.lib.active.read_bytes(),self.new)
        self.assertEqual(Path(result['backup']).read_bytes(),self.old)
        self.assertEqual((self.root/'pdp23drum.bak').read_bytes(),self.old)
        self.assertEqual(self.lib.active.stat().st_mode&0o777,before_mode)
        restored=self.lib.restore()
        self.assertEqual(self.lib.active.read_bytes(),self.old)
        self.assertEqual(Path(restored['backup']).read_bytes(),self.new)
        self.assertIsNone(self.lib.read_state()['current'])
        self.assertEqual(self.rt.current,'halted')
    def test_repeat_pick_is_noop_and_keeps_original_backup(self):
        first=self.lib.pick(self.image)
        journal=self.lib.journal.read_bytes()
        again=self.lib.pick(self.image)
        self.assertFalse(again['changed'])
        self.assertEqual(self.lib.journal.read_bytes(),journal)
        self.assertEqual(Path(first['backup']).read_bytes(),self.old)
    def test_atomic_publish_failure_keeps_original_and_recovers(self):
        write=drum.atomic_write
        def fail_active(path,*a,**kw):
            if Path(path)==self.lib.active: raise OSError('simulated disk failure')
            return write(path,*a,**kw)
        with patch.object(drum,'atomic_write',side_effect=fail_active):
            with self.assertRaises(OSError):self.lib.pick(self.image)
        self.assertEqual(self.lib.active.read_bytes(),self.old)
        self.assertEqual(self.lib.read_state()['history'][-1]['stage'],'prepared')
        result=self.lib.restore()
        self.assertFalse(result['changed'])
        self.assertIsNone(self.lib.read_state()['current'])
    def test_journal_failure_after_publish_can_restore(self):
        write=self.lib.write_state
        calls=0
        def fail_second(state):
            nonlocal calls
            calls+=1
            if calls==2: raise OSError('simulated journal failure')
            return write(state)
        with patch.object(self.lib,'write_state',side_effect=fail_second):
            with self.assertRaises(OSError):self.lib.pick(self.image)
        self.assertEqual(self.lib.active.read_bytes(),self.new)
        self.assertEqual(self.lib.read_state()['history'][-1]['stage'],'prepared')
        self.lib.restore()
        self.assertEqual(self.lib.active.read_bytes(),self.old)
    def test_changed_backup_refused(self):
        result=self.lib.pick(self.image)
        Path(result['backup']).write_bytes(self.new)
        with self.assertRaises(drum.DrumError):self.lib.restore()
        self.assertEqual(self.lib.active.read_bytes(),self.new)
    def test_concurrent_active_change_refused(self):
        write_state=self.lib.write_state
        other=struct.pack('<I',0o222222)*(32*4096)
        def change(state):
            write_state(state)
            self.lib.active.write_bytes(other)
        with patch.object(self.lib,'write_state',side_effect=change):
            with self.assertRaises(drum.DrumError):self.lib.pick(self.image)
        self.assertEqual(self.lib.active.read_bytes(),other)
    def test_emulator_resumes_during_preparation_refused(self):
        write_state=self.lib.write_state
        def resume(state):
            write_state(state)
            self.rt.current='running'
        with patch.object(self.lib,'write_state',side_effect=resume):
            with self.assertRaises(drum.DrumError):self.lib.pick(self.image)
        self.assertEqual(self.lib.active.read_bytes(),self.old)
    def test_active_symlink_refused(self):
        other=self.root/'other.drm';other.write_bytes(self.old)
        self.lib.active.unlink();self.lib.active.symlink_to(other)
        with self.assertRaises(drum.DrumError):self.lib.pick(self.image)
        self.assertEqual(other.read_bytes(),self.old)
    def test_bad_size_and_high_bits_refused(self):
        for raw in (b'',b'123',b'1234'*(32*4096+1),struct.pack('<I',1<<18)):
            with self.assertRaises(drum.DrumError):drum.describe(raw)
    def test_prepare_switch_reasserts_stop(self):
        runtime=drum.Runtime(self.root)
        with patch.object(runtime,'state',return_value='halted'), \
             patch.object(drum.socket,'create_connection') as connect:
            connection=connect.return_value.__enter__.return_value
            connection.recv.return_value=b'ok\n'
            runtime.prepare_switch()
            connection.sendall.assert_called_once_with(b'halt\n')
    def test_prepare_switch_does_not_touch_power_off(self):
        runtime=drum.Runtime(self.root)
        with patch.object(runtime,'state',return_value='power off'), \
             patch.object(drum.socket,'create_connection') as connect:
            runtime.prepare_switch()
            connect.assert_not_called()
    def test_prepare_switch_refuses_running(self):
        runtime=drum.Runtime(self.root)
        with patch.object(runtime,'state',return_value='running'), \
             patch.object(drum.socket,'create_connection') as connect:
            with self.assertRaises(drum.DrumError):runtime.prepare_switch()
            connect.assert_not_called()

    def test_runtime_detects_open_fd(self):
        # Exercise real /proc fd detection in this disposable process, not the live emulator.
        if not Path('/proc').is_dir():self.skipTest('Linux procfs check runs on the Pi')
        runtime=drum.Runtime(self.root)
        with patch.object(runtime,'pids',return_value=[__import__('os').getpid()]), \
             patch.object(runtime,'require_stopped',return_value='halted'):
            with self.lib.active.open('rb'):
                with self.assertRaises(drum.DrumError):runtime.wait_closed(self.lib.active,timeout=0)
            runtime.wait_closed(self.lib.active,timeout=0)

if __name__=='__main__':unittest.main(verbosity=2)
