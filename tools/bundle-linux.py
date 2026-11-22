#!/usr/bin/env python3
"""仅用于部署：收集动态依赖，主程序不需要 Python。基线 ABI 取决于构建机。"""
import pathlib, subprocess, shutil, re, os, sys, json
root = pathlib.Path(sys.argv[1]).resolve()
libs = root/'lib'; libs.mkdir(exist_ok=True)
plugins = root/'plugins'; plugins.mkdir(exist_ok=True)
# Qt 安装目录（系统 Qt 或本地开发前缀）。
qt_candidates=[pathlib.Path('/usr/lib/x86_64-linux-gnu/qt6/plugins')]
qmake=shutil.which('qmake6')
if qmake:
    result=subprocess.run([qmake,'-query','QT_INSTALL_PLUGINS'],capture_output=True,text=True)
    qt_candidates.insert(0,pathlib.Path(result.stdout.strip()))
    # qmake from an extracted Debian/Ubuntu prefix may report the system path.
    if result.returncode == 0:
        qt_candidates.insert(0,pathlib.Path('.deps') / result.stdout.strip().lstrip('/'))
qt=next((p for p in qt_candidates if (p/'platforms').is_dir()),None)
if not qt: raise SystemExit('找不到 Qt 平台插件')
queue=[root/'bin/snapling']
for category in ['platforms','platformthemes','imageformats','platforminputcontexts','wayland-graphics-integration-client',
                 'wayland-shell-integration','wayland-decoration-client']:
    path=qt/category
    if path.exists():
        dest=plugins/category;dest.mkdir(exist_ok=True)
        for plugin in path.glob('*.so'):
            if plugin.name in ['libqeglfs.so','libqvkkhrdisplay.so','libqlinuxfb.so','libqvnc.so']:continue
            shutil.copy2(plugin,dest/plugin.name);queue.append(dest/plugin.name)
seen=set()
excluded=re.compile(r'^(libc\.so|libm\.so|libpthread\.so|libdl\.so|librt\.so|ld-linux|libresolv\.so)')
# 不带上可选 EGL 插件的 GPU 驱动；这些由目标系统提供。
while queue:
    binary=queue.pop()
    result=subprocess.run(['ldd',str(binary)],capture_output=True,text=True)
    if 'not found' in result.stdout:raise SystemExit(result.stdout)
    for name,path in re.findall(r'^\s*(\S+) => (/.+?)\s+\(0x[0-9a-fA-F]+\)\s*$',result.stdout,re.M):
        if name in seen or excluded.match(name):continue
        seen.add(name);dest=libs/name
        if pathlib.Path(path).resolve()!=dest.resolve():shutil.copy2(path,dest,follow_symlinks=True)
        queue.append(dest)
(root/'bin/qt.conf').write_text('[Paths]\nPrefix=..\nPlugins=plugins\n')
launcher=root/'snapling'
launcher.write_text('''#!/bin/sh
SNAPLING_BUNDLE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$SNAPLING_BUNDLE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$SNAPLING_BUNDLE/plugins"
exec "$SNAPLING_BUNDLE/bin/snapling" "$@"
''')
launcher.chmod(0o755)
# LGPL 动态库保持可替换，并附带可取得的发行版版权文件。
licenses=root/'share/licenses';licenses.mkdir(parents=True,exist_ok=True)
for prefix in [pathlib.Path('/usr/share/doc'),pathlib.Path('.deps/usr/share/doc')]:
    for name in ['qt6-base-dev','libqt6core6t64','libqt6gui6','libqt6widgets6','libqt6network6',
                 'qt6-wayland','libtesseract5','libtesseract-dev','libpipewire-0.3-0t64',
                 'libzxing3','libssl3t64','libstdc++6','libicu78','libxcb1','libx11-6']:
        p=prefix/name/'copyright'
        if p.exists():shutil.copy2(p,licenses/(name+'.txt'),follow_symlinks=True)
for name in ['LGPL-3','LGPL-2.1','GPL-3','Apache-2.0']:
    p=pathlib.Path('/usr/share/common-licenses')/name
    if p.exists():shutil.copy2(p,licenses/name)
print(f'Bundled {len(seen)} shared libraries into {root}')

# 按实际分发的库收集发行版版权与版本，覆盖传递依赖。
packages=set()
for library in sorted(seen):
    result=subprocess.run(['dpkg-query','-S','*/'+library],capture_output=True,text=True)
    for line in result.stdout.splitlines():
        if ': ' in line:packages.add(line.split(': ',1)[0].split(':',1)[0])
metadata=[]
for package in sorted(packages):
    result=subprocess.run(['dpkg-query','-W','-f=${Version}',package],capture_output=True,text=True)
    metadata.append({'package':package,'version':result.stdout.strip()})
    copyright=pathlib.Path('/usr/share/doc')/package/'copyright'
    if copyright.exists():shutil.copy2(copyright,licenses/(package+'.txt'),follow_symlinks=True)
# 本地提取的少数运行包尚未登记在主机 dpkg 数据库中。
for copyright in pathlib.Path('.deps/usr/share/doc').glob('*/copyright'):
    if copyright.exists():shutil.copy2(copyright,licenses/(copyright.parent.name+'.txt'),follow_symlinks=True)
(licenses/'DEPENDENCIES.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2)+'\n')
print(f'Collected copyright and versions for {len(packages)} distribution packages')
