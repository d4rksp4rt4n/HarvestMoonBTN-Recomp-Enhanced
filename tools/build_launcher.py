#!/usr/bin/env python3
"""Build dist-template/HarvestMoonBTN-Launcher.exe from dist-template/launcher.pyw (PyInstaller).

The exe carries the build scripts and the runtime sources (nothing of the game), so its
"Build from my disc" page can make the game anywhere: give a friend the launcher alone and they
build from their own disc. Rebuild it after changing launcher.pyw, the runtime or the tools.

usage: python tools/build_launcher.py      (needs: pip install pyinstaller)
"""
import os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TPL = os.path.join(ROOT, 'dist-template')


def main():
    work = os.path.join(ROOT, 'build', 'launcher')
    src = os.path.join(work, 'src')
    shutil.rmtree(work, ignore_errors=True)
    skip = shutil.ignore_patterns('__pycache__', 'tests', 'HarvestMoonBTN-Launcher.exe')
    shutil.copytree(os.path.join(ROOT, 'tools'), os.path.join(src, 'tools'), ignore=skip)
    shutil.copytree(os.path.join(ROOT, 'runtime'), os.path.join(src, 'runtime'), ignore=skip)
    shutil.copytree(TPL, os.path.join(src, 'dist-template'), ignore=skip)
    shutil.copy2(os.path.join(ROOT, 'build.py'), src)
    subprocess.check_call([sys.executable, '-m', 'PyInstaller', '--noconfirm', '--onefile', '--windowed',
                           '--name', 'HarvestMoonBTN-Launcher',
                           '--icon', os.path.join(TPL, 'launcher-assets', 'icon.ico'),
                           '--add-data', src + os.pathsep + 'src', '--distpath', TPL, '--workpath', work,
                           '--specpath', work, os.path.join(TPL, 'launcher.pyw')])
    print('built', os.path.join(TPL, 'HarvestMoonBTN-Launcher.exe'))


if __name__ == '__main__':
    main()
