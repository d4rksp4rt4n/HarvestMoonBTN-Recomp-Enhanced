#!/usr/bin/env python3
"""Publish the launcher on GitHub as release v<VERSION>, with HarvestMoonBTN-Launcher.exe attached.
Players' launchers find it, update themselves and rebuild the game from their disc's files.

  1. raise VERSION in dist-template/launcher.pyw
  2. python tools/build_launcher.py, commit, merge into main
  3. on main, up to date:  python tools/release.py ["what changed"]      (needs gh, logged in)

The release holds only the launcher (its scripts and sources): nothing of the game.
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = 'd4rksp4rt4n/HarvestMoonBTN-Recomp-Enhanced'
EXE = os.path.join(ROOT, 'dist-template', 'HarvestMoonBTN-Launcher.exe')


def git(*a):
    return subprocess.run(['git', '-C', ROOT, *a], capture_output=True, text=True).stdout.strip()


def main():
    src = open(os.path.join(ROOT, 'dist-template', 'launcher.pyw'), encoding='utf-8').read()
    version = re.search(r"^VERSION = '([\d.]+)'", src, re.M).group(1)
    tag = 'v' + version
    # the exe must be the one built from this launcher.pyw (it prints its VERSION)
    built = subprocess.run([EXE, '--version'], capture_output=True, text=True,
                           env=dict(os.environ, PYINSTALLER_RESET_ENVIRONMENT='1')).stdout.strip()
    if built != version:
        sys.exit('the launcher exe is %s, launcher.pyw says %s: run tools/build_launcher.py and commit' % (built, version))
    if git('status', '--porcelain', '--untracked-files=no'):
        sys.exit('uncommitted changes: commit them first')
    git('fetch', '--quiet', 'origin', 'main')
    if git('rev-parse', 'HEAD') != git('rev-parse', 'origin/main'):
        sys.exit('release from main as it is on GitHub (git checkout main && git pull)')
    if git('ls-remote', '--tags', 'origin', tag):
        sys.exit('%s exists already: raise VERSION in launcher.pyw' % tag)
    notes = sys.argv[1] if len(sys.argv) > 1 else 'Launcher %s.' % version
    notes += ('\n\nPut HarvestMoonBTN-Launcher.exe in a folder and open it; its "Build from my disc" page makes '
              'the game from your own disc image. A launcher already in use updates itself.')
    subprocess.check_call(['gh', 'release', 'create', tag, EXE, '--repo', REPO, '--target', git('rev-parse', 'HEAD'),
                           '--title', 'Launcher ' + tag, '--notes', notes])


if __name__ == '__main__':
    main()
