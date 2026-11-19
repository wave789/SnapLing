#!/usr/bin/env python3
"""真实主程序端到端：外部窗口/菜单 → IPC 截图 → GUI 标注 → GUI 保存。"""
import os,pathlib,subprocess,tempfile,time,shutil,struct
root=pathlib.Path(__file__).resolve().parents[1]
app=str(root/'build/snapling');fixture=str(root/'build/snapling_desktop_test')
with tempfile.TemporaryDirectory(prefix='snapling-app-e2e-') as tmp:
    env=os.environ.copy();env['XDG_CONFIG_HOME']=tmp;env['QT_USE_PORTAL']='0';env['QT_QPA_PLATFORMTHEME']='generic';env['QT_IM_MODULE']='compose'
    config=pathlib.Path(tmp)/'SnapLing/SnapLing/settings.ini';config.parent.mkdir(parents=True);config.write_text('[General]\nschemaVersion=1\n[%general]\nsaveDir='+tmp+'\n')
    def ctl(*args):return subprocess.check_output(['xdotool',*args],env=env,text=True,timeout=5).strip()
    processes=[]
    try:
        main=subprocess.Popen([app,'--background','--exit-after','18000'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True);processes.append(main)
        line=main.stderr.readline();assert 'startup_ms=' in line,line
        target=subprocess.Popen([fixture,'--target-only'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True);processes.append(target)
        target_window=ctl('search','--sync','--name','^SnapLing real desktop target$').splitlines()[0];time.sleep(.3)
        subprocess.run([app,'--full'],env=env,check=True,timeout=5)
        window=ctl('search','--sync','--name','^SnapLing · 截图编辑$').splitlines()[0]
        ctl('key','--window',target_window,'Escape');ctl('windowfocus',window)
        ctl('mousemove','--window',window,'170','18');ctl('click','1')
        ctl('mousemove','--window',window,'320','330');ctl('mousedown','1');ctl('mousemove','--window',window,'620','400');ctl('mouseup','1')
        ctl('key','--window',window,'ctrl+s')
        dialog=ctl('search','--sync','--name','^保存图片$').splitlines()[0]
        ctl('windowfocus',dialog);ctl('key','--window',dialog,'ctrl+a')
        output=pathlib.Path(tmp)/'application-output.png'
        ctl('type','--window',dialog,'--clearmodifiers',str(output));time.sleep(.2)
        ctl('key','Return')
        deadline=time.monotonic()+5
        while not output.exists() and time.monotonic()<deadline:time.sleep(.05)
        
        assert output.exists(),'GUI save did not create image'
        header=output.read_bytes();assert header[:8]==b'\x89PNG\r\n\x1a\n'
        assert struct.unpack('>II',header[16:24])==(1920,1080)
        from PIL import Image
        with Image.open(output) as image:
            colors=image.convert('RGB').getdata()
            red=sum(r>180 and g<120 and b<120 for r,g,b in colors)
        assert red>100,('annotation missing',red)
        dest=root/'test-artifacts/application-ui-saved.png';shutil.copy2(output,dest)
        subprocess.run([app,'--quit'],env=env,check=True,timeout=5);assert main.wait(timeout=5)==0
        print('actual_application_capture_edit_save=PASS; annotation_red_pixels='+str(red))
    finally:
        for process in reversed(processes):
            if process.poll() is None:process.terminate();process.wait(timeout=5)
