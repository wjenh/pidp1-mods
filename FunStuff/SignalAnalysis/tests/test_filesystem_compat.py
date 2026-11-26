#!/usr/bin/env python3
"""Real fstool/mtfs cross-reading and host entry points; only temporary media."""
import hashlib,json,os,subprocess,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from tape import filesystem_tool,make_image,from_drum,selected_recording,write_config

ROOT=Path(__file__).resolve().parents[1]


def main():
    installation=Path(os.environ.get('PDP1_ROOT','/opt/pidp1-mods'))
    tools=[installation/'bin'/n for n in ('fstool','mtfs')]
    assert all(t.is_file() and os.access(t,os.X_OK) for t in tools),'Cross-version check requires both tools'
    recordings=[('APOLLO11',ROOT/'samples/Apollo11.drm'),('TWOTONE',ROOT/'samples/SignalAnalysis.drm'),('CHANGING',ROOT/'samples/SignalAnalysis-Changing.drm')]
    checks=[]
    with tempfile.TemporaryDirectory(prefix='saw-fstool-compat-') as td:
        d=Path(td);root=d/'installation';(root/'bin').mkdir(parents=True)
        for tool in tools:(root/'bin'/tool.name).symlink_to(tool)
        assert filesystem_tool(root)==root/'bin/fstool'
        for writer in tools:
            image=d/(writer.name+'.mtp')
            report=make_image(image,recordings,writer)
            assert report['read_only'] and image.stat().st_mode&0o222==0
            before=hashlib.sha256(image.read_bytes()).hexdigest()
            for reader in tools:
                for name,source in recordings:
                    out=d/(writer.name+'-'+reader.name+'-'+name+'.words')
                    subprocess.run([str(reader),'-x','-n',name,'-f','words','-o',str(out),str(image)],check=True)
                    assert out.read_bytes()==from_drum(source.read_bytes())
                checks.append(writer.name+' -> '+reader.name)
            assert hashlib.sha256(image.read_bytes()).hexdigest()==before
            home=d/'config';home.mkdir(exist_ok=True)
            write_config(home,image=str(image))
            _,selected,info=selected_recording(root,home)
            assert selected==image and info['valid_samples']==36764
        # Exercise real CLI --root discovery, then the explicit legacy override.
        for override in ([],['--tool',str(tools[1])]):
            output=d/('override.mtp' if override else 'auto.mtp')
            result=subprocess.run([sys.executable,str(ROOT/'tape.py'),'pack',str(output),'--root',str(root),
                '--recording','TWOTONE='+str(recordings[1][1]),*override],check=True,capture_output=True,text=True)
            report=json.loads(result.stdout)
            assert Path(report['tool']).name==('mtfs' if override else 'fstool')
            assert output.is_file() and output.stat().st_mode&0o222==0
        print(json.dumps({'cross_readings':checks,'all_three_recordings_identical':True,
            'read_only_images':True,'selected_recording_prefers_fstool':True,
            'CLI_custom_root_and_override':True,'live_devices_accessed':False},indent=2))

if __name__=='__main__':main()
