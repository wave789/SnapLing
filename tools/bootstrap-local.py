#!/usr/bin/env python3
"""开发机无 sudo 时，从 apt 源提取依赖；不修改系统安装。"""
import concurrent.futures
import hashlib
import pathlib
import re
import subprocess
import urllib.request
root = pathlib.Path(__file__).resolve().parents[1] / '.deps'
root.mkdir(exist_ok=True)
(root / 'debs').mkdir(exist_ok=True)
packages = ['pkg-config','libxkbcommon-dev','qt6-base-dev','qt6-base-dev-tools','qt6-svg-dev','libtesseract-dev',
            'tesseract-ocr-eng','tesseract-ocr-chi-sim','libpipewire-0.3-dev',
            'libzxing-dev','libx11-dev','libxrandr-dev','libxcb1-dev','qt6-wayland','xvfb','xdotool']
plan = subprocess.check_output(['apt-get','--print-uris','--yes','--download-only',
                               '-o','Debug::NoLocking=1','install',*packages],text=True)
(root/'packages.txt').write_text(plan)
items = re.findall(r"^'([^']+)' (\S+)",plan,re.M)
def download(item):
    url,name = item
    dest = root/'debs'/name
    urllib.request.urlretrieve(url,dest)
    package = name.split('_')[0]
    metadata = subprocess.check_output(['apt-cache','show',package],text=True)
    expected = re.search(r'^SHA256: (\w+)',metadata,re.M)
    if not expected or hashlib.sha256(dest.read_bytes()).hexdigest() != expected[1]:
        raise RuntimeError(f'校验失败: {name}')
    return dest
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
    archives = list(pool.map(download,items))
for archive in archives:
    subprocess.run(['dpkg-deb','-x',str(archive),str(root)],check=True)
    print(archive.name)

# 将开发包引用到已经安装的同版本系统运行库；保留完整本地前缀。
local_usr=root/'usr'
for p in (local_usr/'lib/x86_64-linux-gnu/cmake').rglob('*Targets*.cmake'):
    for name in re.findall(r'\$\{_IMPORT_PREFIX\}/(lib/[^";]+)',p.read_text()):
        dest=local_usr/name
        system=pathlib.Path('/usr')/name
        if not dest.exists() and system.exists():
            dest.parent.mkdir(parents=True,exist_ok=True)
            dest.symlink_to(system)
for p in (local_usr/'lib/x86_64-linux-gnu').glob('*.so'):
    if p.is_symlink() and not p.exists():
        target=pathlib.Path('/usr/lib/x86_64-linux-gnu')/p.readlink()
        if target.exists():
            dest=p.parent/p.readlink()
            if not dest.exists():dest.symlink_to(target)
for p in (local_usr/'lib/x86_64-linux-gnu/pkgconfig').glob('*.pc'):
    text=p.read_text().replace('prefix=/usr',f'prefix={local_usr}').replace('includedir=/usr/include',f'includedir={local_usr}/include').replace('libdir=/usr/lib',f'libdir={local_usr}/lib')
    p.write_text(text)
