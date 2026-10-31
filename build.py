#!/usr/bin/env python3
"""Build the native port from YOUR OWN copy of Harvest Moon: Back to Nature (USA, SLUS-01115).

  1. reads the game executable and its code overlays from your disc image -> extract/
  2. statically recompiles the PlayStation (MIPS) code to C               -> generated/
  3. compiles the runtime + the generated code into a native program      -> build/
  4. puts a ready-to-play folder together                                  -> dist/

usage: python build.py [--disc PATH] [--pak] [--zip] [--into DIR] [--linux] [--debug] [--jobs N] [--clean]

  --disc PATH  your disc image (.cue or .bin); default: the first .cue (or .bin) in disc/
  --pak        also turn your disc image into the compact data pack HarvestMoonBTN.pak and put it
               in the dist folder, so the game runs without the disc image
  --zip        also create a .zip of the dist folder
  --into DIR   put the game into DIR (a folder you play from) instead of dist/; nothing there is
               deleted (the launcher's "Build from my disc" page uses it)
  --linux      on Windows: cross-build the Linux version (run "python tools/fetch_deps.py --linux")
  --debug      unoptimised build with a console window, for development
  --clean      rebuild everything from scratch

Windows needs the toolchain in deps/ first:  python tools/fetch_deps.py
Linux needs a C compiler and the SDL2 development package (e.g. apt install build-essential libsdl2-dev).
"""
import glob, hashlib, os, platform, subprocess, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import compile as native            # noqa: E402
from iso import Disc, find_file, resolve_bin   # noqa: E402
from package import package         # noqa: E402

# the files the recompiled code is generated from, and their SHA-256 for the USA release
GAME_FILES = {
    'SLUS_011.15': '34e3b35dcd3fb1cd9a7d4813b8a9ef987915826e3d436f1908ecbe2d4fdf1444',
    'A_FILE.HDT': '0cae44d6bdb29e078a182d7cd9a780c21e35dc54f55eb94c2d9ea8deb20c82dd',
    'A_FILE.BIN': '612f9ac8279a364c2fdd19cb75e63df2e892a6f52b6f1d8081ea8560b18decd4',
}
# inputs of the recompiler; generated/ is redone when any of them changes
RECOMP_INPUTS = ['tools/recomp.py', 'tools/mips.py', 'tools/overlays.json', 'tools/extra_entries.json']


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def extracted():
    """the game files are in extract/ already, verified"""
    out = os.path.join(ROOT, 'extract')
    return all(os.path.exists(os.path.join(out, n)) and sha256_file(os.path.join(out, n)) == h
               for n, h in GAME_FILES.items())


def find_disc(arg, need=True):
    if arg:
        if not os.path.isfile(arg):
            sys.exit('disc image not found: ' + arg)
        return os.path.abspath(arg)
    for ext in ('*.cue', '*.bin'):
        hits = sorted(glob.glob(os.path.join(ROOT, 'disc', ext)))
        if hits:
            return hits[0]
    if not need:
        return None                 # a rebuild without the disc image, from extract/
    sys.exit('No disc image found.\n'
             'Put your own Harvest Moon: Back to Nature (USA) .cue + .bin into the "disc" folder,\n'
             'or pass it with:  python build.py --disc "path/to/Harvest Moon - Back to Nature.cue"')


def extract(disc_path):
    """copy the game files the recompiler needs out of the disc image and verify them"""
    out = os.path.join(ROOT, 'extract')
    os.makedirs(out, exist_ok=True)
    if extracted():
        return
    binpath = resolve_bin(disc_path)
    if not os.path.isfile(binpath):
        sys.exit('the .bin file named in the .cue sheet is missing: ' + binpath)
    if os.path.getsize(binpath) % 2352:
        sys.exit('%s is not a raw Mode2/2352 image (.bin from a .cue/.bin dump)' % binpath)
    disc = Disc(binpath)
    print('reading game files from', binpath)
    for name, want in GAME_FILES.items():
        e = find_file(disc, name)
        if not e:
            sys.exit('%s is not on this disc - this is not Harvest Moon: Back to Nature (USA)' % name)
        data = disc.read_extent(e['lba'], e['size'])
        if hashlib.sha256(data).hexdigest() != want:
            sys.exit('%s on this disc does not match the USA release (SLUS-01115).\n'
                     'Only the original USA version is supported; check that your dump is good.' % name)
        with open(os.path.join(out, name), 'wb') as f:
            f.write(data)


def recompile(clean):
    h = hashlib.sha256()
    for n in sorted(GAME_FILES):
        h.update(GAME_FILES[n].encode())
    for p in RECOMP_INPUTS:
        # as text: Windows line ends (a copy not checked out with git) must not change the key, which
        # goes into the co-op build id (tools/compile.py)
        h.update(open(os.path.join(ROOT, p), 'rb').read().replace(b'\r\n', b'\n'))
    key = h.hexdigest()
    stamp = os.path.join(ROOT, 'generated', '.stamp')
    if not clean and os.path.exists(stamp) and open(stamp).read() == key:
        return
    print('recompiling the game code (MIPS -> C) ...')
    os.makedirs(os.path.join(ROOT, 'generated'), exist_ok=True)
    os.makedirs(os.path.join(ROOT, 'build'), exist_ok=True)
    for old in glob.glob(os.path.join(ROOT, 'generated', '*')):
        os.remove(old)
    subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'recomp.py')])
    with open(stamp, 'w') as f:
        f.write(key)


def make_pak(disc_path):
    pak = os.path.join(ROOT, 'build', 'HarvestMoonBTN.pak')
    src = resolve_bin(disc_path)
    stamp = pak + '.src'
    key = '%s|%d|%d' % (os.path.abspath(src), os.path.getsize(src), int(os.path.getmtime(src)))
    if not (os.path.exists(pak) and os.path.exists(stamp) and open(stamp).read() == key):
        print('building the data pack from your disc image ...')
        subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'makepak.py'), disc_path, pak])
        with open(stamp, 'w') as f:
            f.write(key)
    return pak


def behind_main():
    """warn when this git checkout lacks commits of main on GitHub: online co-op only connects to a
    game built from the same sources, so a build of an older branch can't play with the others'"""
    git = lambda *a: subprocess.run(['git', '-C', ROOT, *a], capture_output=True, text=True, timeout=30,
                                    creationflags=native.NO_WINDOW)
    try:
        if git('rev-parse', '--is-inside-work-tree').stdout.strip() != 'true':
            return
        remotes = git('remote').stdout.split()
        remote = 'upstream' if 'upstream' in remotes else 'origin' if 'origin' in remotes else None
        if not remote or git('fetch', '--quiet', remote, 'main').returncode:
            return                                  # offline, or no main there: nothing to compare with
        missing = git('log', '--oneline', '--no-merges', 'HEAD..%s/main' % remote).stdout.splitlines()
    except (OSError, subprocess.SubprocessError):
        return                                      # no git
    if missing:
        print('\nWARNING: this checkout lacks %d commit(s) of %s/main:' % (len(missing), remote))
        for line in missing[:8]:
            print('   ', line)
        if len(missing) > 8:
            print('    ...')
        print('Online co-op only connects to a game built from the same sources: a friend who builds main\n'
              'gets another version. To have them: git merge %s/main\n' % remote)


def main():
    args = sys.argv[1:]
    if '-h' in args or '--help' in args:
        print(__doc__)
        return
    opt = lambda n: n in args
    disc_arg = args[args.index('--disc') + 1] if '--disc' in args else None
    jobs = int(args[args.index('--jobs') + 1]) if '--jobs' in args else None
    linux = opt('--linux') or platform.system() != 'Windows'

    behind_main()
    disc_path = find_disc(disc_arg, need=opt('--pak') or not extracted())
    print('disc image:', disc_path or 'none (the game files from it are in extract/)')
    extract(disc_path)
    recompile(opt('--clean'))
    exe = native.build(release=not opt('--debug'), linux_cross=opt('--linux'), clean=opt('--clean'), jobs=jobs)
    pak = make_pak(disc_path) if opt('--pak') else None

    files = [(exe, os.path.basename(exe))]
    if exe.endswith('.exe'):
        files.append((os.path.join(os.path.dirname(exe), 'SDL2.dll'), 'SDL2.dll'))
    name = 'HarvestMoonBTN-%s-x64%s' % ('Linux' if linux else 'Windows', '-debug' if opt('--debug') else '')
    # remember the disc image location in hmbtn.ini, unless the build is for another machine
    into = args[args.index('--into') + 1] if '--into' in args else None
    out = package(name, files, pak=pak, disc_path=None if opt('--linux') else disc_path, make_zip=opt('--zip'),
                  into=into)
    print('\nDone. Play:  %s' % os.path.join(out, os.path.basename(exe)))


if __name__ == '__main__':
    main()
