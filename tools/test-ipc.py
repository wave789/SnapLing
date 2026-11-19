#!/usr/bin/env python3
"""在独立配置目录验证实际主程序的单实例命令往返。"""
import os,pathlib,subprocess,sys,tempfile,time
exe=str(pathlib.Path(sys.argv[1] if len(sys.argv)>1 else 'build/snapling').resolve())
with tempfile.TemporaryDirectory(prefix='snapling-ipc-') as config:
    env=os.environ.copy();env['XDG_CONFIG_HOME']=config
    started=time.monotonic()
    process=subprocess.Popen([exe,'--background','--exit-after','10000'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        line=process.stderr.readline()
        if 'startup_ms=' not in line:raise RuntimeError(line)
        result=subprocess.run([exe,'--quit'],env=env,capture_output=True,text=True,timeout=5)
        assert result.returncode==0,result.stderr
        assert process.wait(timeout=5)==0
        assert time.monotonic()-started<8
        print('single_instance_command_roundtrip=PASS')
    finally:
        if process.poll() is None:process.terminate();process.wait(timeout=5)
