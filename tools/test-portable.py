#!/usr/bin/env python3
"""在独立 X11 桌面运行便携包，实际触发 OCR；只使用测试截图。"""
import os,pathlib,subprocess,tempfile,sys,shutil
source=pathlib.Path(sys.argv[1]).resolve()
image=pathlib.Path('test-artifacts/desktop-edited.png').resolve()
assert image.exists()
with tempfile.TemporaryDirectory(prefix='snapling-relocated-') as temp:
    root=pathlib.Path(temp)
    shutil.copytree(source,root/'bundle')
    env=os.environ.copy()
    for key in ['LD_LIBRARY_PATH','QT_PLUGIN_PATH','TESSDATA_PREFIX']:env.pop(key,None)
    env['XDG_CONFIG_HOME']=str(root/'config')
    env['QT_QPA_PLATFORM']='xcb';env['QT_QPA_PLATFORMTHEME']='generic';env['QT_USE_PORTAL']='0'
    exe=root/'bundle/snapling'
    process=subprocess.Popen([str(exe),'--open',str(image),'--exit-after','6000'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        window=subprocess.check_output(['xdotool','search','--sync','--name','^SnapLing · 截图编辑$'],env=os.environ,text=True,timeout=5).splitlines()[0]
        subprocess.run(['xdotool','windowfocus',window],env=os.environ,check=True)
        subprocess.run(['xdotool','key','--window',window,'ctrl+shift+o'],env=os.environ,check=True)
        out,error=process.communicate(timeout=12)
        assert process.returncode==0,error
        assert 'ocr_ms=' in error,error
        assert '.deps' not in error,error
        print('relocated_bundle_gui_ocr=PASS')
        print(error)
    finally:
        if process.poll() is None:process.terminate();process.wait(timeout=5)
