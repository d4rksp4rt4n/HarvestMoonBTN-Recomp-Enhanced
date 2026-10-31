#!/usr/bin/env python3
"""Write tools/toolchain.txt: the files of deps/ (from tools/fetch_deps.py) that a Windows build
really reads, with their SHA-256. "fetch_deps.py --min" downloads only those (the launcher's
"Build from my disc" page uses it), about a tenth of the full toolchain.

Run it again after changing a #include in runtime/, the link libraries or a pinned toolchain
version: it does a clean build with the full deps/ and records each header the compiler reads
(-MD) and each file the linker reads (--reproduce).

usage: python tools/min_toolchain.py
"""
import glob, hashlib, os, sys, tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import compile as native            # noqa: E402

DEPS = os.path.join(ROOT, 'deps')
# the programs (and what they load) the build runs; the compiler and linker read the rest
PROGRAMS = ['llvm-mingw/bin/' + n for n in (
    'clang.exe', 'clang-23.exe', 'clang-target-wrapper.exe', 'x86_64-w64-mingw32-clang.exe', 'ld.lld.exe',
    'x86_64-w64-mingw32-windres.exe', 'libLLVM-23.dll', 'libclang-cpp.dll', 'libc++.dll', 'libunwind.dll',
    'libwinpthread-1.dll', 'mingw32-common.cfg', 'x86_64-w64-windows-gnu.cfg', 'x86_64-pc-windows-gnu.cfg')] + [
    'SDL2-2.32.10/x86_64-w64-mingw32/bin/SDL2.dll']


def main():
    repro = os.path.join(ROOT, 'build', 'link-repro.tar')
    sh = native.sh

    def traced(cmd):
        if '-c' in cmd:
            cmd = cmd + ['-MD']                         # writes x.d next to x.o
        elif '-o' in cmd and cmd[cmd.index('-o') + 1].endswith('.exe'):      # the link
            cmd = cmd + ['-Wl,--reproduce=' + repro]
        return sh(cmd)
    native.sh = traced
    native.build(release=True, linux_cross=False, clean=True, jobs=None)

    used = set(PROGRAMS)
    deps = os.path.realpath(DEPS)                       # may be a link to another checkout's

    def add(path):
        p = os.path.realpath(path)
        if os.path.normcase(p).startswith(os.path.normcase(deps) + os.sep):
            used.add(os.path.relpath(p, deps).replace('\\', '/'))
    for d in glob.glob(os.path.join(ROOT, 'build', '**', '*.d'), recursive=True):
        text = open(d).read().replace('\\\n', ' ')
        for tok in text.split('.o:', 1)[1].split():    # "x.o: x.c headers..."
            add(tok)
        os.remove(d)
    with tarfile.open(repro) as t:
        for m in t.getmembers():                        # <name>/C/Users/.../deps/llvm-mingw/...
            parts = m.name.split('/', 2)
            if m.isfile() and len(parts) == 3 and len(parts[1]) == 1:
                add(parts[1] + ':/' + parts[2])
    os.remove(repro)

    # case-correct names as in the archives (the .d files may spell them as the source did)
    real = {}
    for dirpath, _, names in os.walk(DEPS):
        for n in names:
            rel = os.path.relpath(os.path.join(dirpath, n), DEPS).replace('\\', '/')
            real[rel.lower()] = rel
    lines = []
    for rel in sorted({real[u.lower()] for u in used}):
        h = hashlib.sha256(open(os.path.join(DEPS, rel), 'rb').read()).hexdigest()
        lines.append('%s  %s\n' % (h, rel))
    with open(os.path.join(ROOT, 'tools', 'toolchain.txt'), 'w', newline='\n') as f:
        f.write('# written by tools/min_toolchain.py: what "fetch_deps.py --min" downloads\n')
        f.writelines(lines)
    size = sum(os.path.getsize(os.path.join(DEPS, l.split('  ', 1)[1].strip())) for l in lines)
    print('%d files, %.1f MB -> tools/toolchain.txt' % (len(lines), size / 1e6))


if __name__ == '__main__':
    main()
