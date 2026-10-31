#!/usr/bin/env python3
"""Download the build toolchain into deps/ (Windows hosts).

  deps/llvm-mingw     clang + LLVM for Windows (github.com/mstorsjo/llvm-mingw)
  deps/SDL2-2.32.10   SDL2 development files for MinGW (window, input, audio)
  with --linux, for cross-building the Linux version on Windows:
  deps/zig            Zig (used as a Linux cross C compiler)
  deps/SDL2-src       SDL2 headers

Every download is checked against a pinned SHA-256.
Linux hosts do not need this: install a C compiler and the SDL2 development package.

--min: only the files a Windows build reads (tools/toolchain.txt, each with its SHA-256), about
65 MB of downloads instead of 205 MB, and 150 MB on disk instead of 830 MB: the launcher's
"Build from my disc" page uses it. Only those bytes of the archives are fetched (HTTP ranges).

usage: python tools/fetch_deps.py [--linux | --min] [--dest DIR]
"""
import hashlib, os, shutil, struct, sys, tarfile, urllib.request, zipfile, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEPS = [
    dict(name='llvm-mingw', top='llvm-mingw-20261006-ucrt-x86_64', linux=False,
         url='https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-x86_64.zip',
         sha256='317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a'),
    dict(name='SDL2-2.32.10', top='SDL2-2.32.10', linux=False,
         url='https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.zip',
         sha256='f15cff5fca62ec9381a016ef1d42a95c638cd72d2f226ba5781c76fe43dbd1ac'),
    dict(name='zig', top='zig-x86_64-windows-0.17.0', linux=True,
         url='https://ziglang.org/download/0.17.0/zig-x86_64-windows-0.17.0.zip',
         sha256='b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428'),
    dict(name='SDL2-src', top='SDL2-2.32.10', linux=True,
         url='https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz',
         sha256='5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165'),
]


def download(url, path):
    with urllib.request.urlopen(url) as r, open(path, 'wb') as f:
        total = int(r.headers.get('Content-Length') or 0)
        done = 0
        while True:
            block = r.read(1 << 20)
            if not block:
                break
            f.write(block)
            done += len(block)
            if total:
                print('\r  %5.1f / %.1f MB' % (done / 1e6, total / 1e6), end='', flush=True)
    print()


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def safe_extract(archive, dest):
    base = os.path.realpath(dest)
    if archive.endswith('.zip'):
        with zipfile.ZipFile(archive) as z:
            for n in z.namelist():
                if not os.path.realpath(os.path.join(dest, n)).startswith(base + os.sep):
                    raise SystemExit('unsafe path in archive: ' + n)
            z.extractall(dest)
    else:
        with tarfile.open(archive) as t:
            for m in t.getmembers():
                if not os.path.realpath(os.path.join(dest, m.name)).startswith(base + os.sep) or m.issym() or m.islnk():
                    raise SystemExit('unsafe entry in archive: ' + m.name)
            if hasattr(tarfile, 'data_filter'):
                t.extractall(dest, filter='data')
            else:
                t.extractall(dest)


def get_range(url, start, end):
    """bytes start..end-1 of url"""
    req = urllib.request.Request(url, headers={'Range': 'bytes=%d-%d' % (start, end - 1)})
    with urllib.request.urlopen(req, timeout=60) as r:
        if r.status != 206:
            raise OSError('the server sent the whole file, not the part asked for')
        return r.read()


def zip_members(url, names):
    """{name: bytes} of these members of the .zip at url, fetching only their bytes"""
    req = urllib.request.Request(url, headers={'Range': 'bytes=0-0'})
    with urllib.request.urlopen(req, timeout=60) as r:
        total = int(r.headers['Content-Range'].split('/')[1])
    tail = get_range(url, max(0, total - 65536), total)
    eocd = tail.rfind(b'PK\x05\x06')
    cd_size, cd_off = struct.unpack_from('<II', tail, eocd + 12)
    cd = get_range(url, cd_off, cd_off + cd_size)
    entries, i = {}, 0
    while cd[i:i + 4] == b'PK\x01\x02':
        method, csize = struct.unpack_from('<H', cd, i + 10)[0], struct.unpack_from('<I', cd, i + 20)[0]
        nlen, xlen, clen = struct.unpack_from('<HHH', cd, i + 28)
        off = struct.unpack_from('<I', cd, i + 42)[0]
        entries[cd[i + 46:i + 46 + nlen].decode('utf-8')] = (off, method, csize)
        i += 46 + nlen + xlen + clen
    missing = [n for n in names if n not in entries]
    if missing:
        raise SystemExit('not in %s: %s' % (url, ', '.join(missing[:5])))
    # each member runs up to the next one's local header; neighbours are fetched in one request
    ends = sorted(o for o, _, _ in entries.values()) + [cd_off]
    nxt = {a: b for a, b in zip(ends, ends[1:])}
    spans = []
    for n in sorted(names, key=lambda n: entries[n][0]):
        off = entries[n][0]
        if spans and off - spans[-1][1] < 1 << 18:
            spans[-1][1] = nxt[off]
            spans[-1][2].append(n)
        else:
            spans.append([off, nxt[off], [n]])
    out, done, size = {}, 0, sum(b - a for a, b, _ in spans)
    for start, end, members in spans:
        blob = get_range(url, start, end)
        done += len(blob)
        print('  %5.1f / %.1f MB' % (done / 1e6, size / 1e6), flush=True)
        for n in members:
            off, method, csize = entries[n]
            p = off - start
            nlen, xlen = struct.unpack_from('<HH', blob, p + 26)
            raw = blob[p + 30 + nlen + xlen:p + 30 + nlen + xlen + csize]
            out[n] = zlib.decompress(raw, -15) if method == 8 else raw
    return out


def fetch_min(dest):
    want = {}                                   # dep name -> {path in it: sha256}
    for line in open(os.path.join(ROOT, 'tools', 'toolchain.txt')):
        if line.strip() and not line.startswith('#'):
            h, rel = line.rstrip().split('  ', 1)
            name, rel = rel.split('/', 1)
            want.setdefault(name, {})[rel] = h
    for d in DEPS:
        files = want.get(d['name'])
        if not files:
            continue
        out = os.path.join(dest, d['name'])
        todo = {r: h for r, h in files.items()
                if not (os.path.isfile(os.path.join(out, r)) and sha256(os.path.join(out, r)) == h)}
        if not todo:
            print('%-14s already present' % d['name'])
            continue
        print('%-14s downloading %d files from %s' % (d['name'], len(todo), d['url']), flush=True)
        got = zip_members(d['url'], [d['top'] + '/' + r for r in todo])
        for r, h in todo.items():
            data = got[d['top'] + '/' + r]
            if hashlib.sha256(data).hexdigest() != h:
                raise SystemExit('checksum mismatch for %s in %s' % (r, d['url']))
            path = os.path.join(out, *r.split('/'))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path + '.part', 'wb') as f:
                f.write(data)
            os.replace(path + '.part', path)
    print('toolchain ready in', dest)


def main():
    linux = '--linux' in sys.argv
    dest = os.path.join(ROOT, 'deps')
    if '--dest' in sys.argv:
        dest = os.path.abspath(sys.argv[sys.argv.index('--dest') + 1])
    os.makedirs(dest, exist_ok=True)
    if '--min' in sys.argv:
        return fetch_min(dest)
    for d in DEPS:
        if d['linux'] and not linux:
            continue
        out = os.path.join(dest, d['name'])
        if os.path.isdir(out):
            print('%-14s already present' % d['name'])
            continue
        archive = os.path.join(dest, os.path.basename(d['url']))
        print('%-14s downloading %s' % (d['name'], d['url']))
        download(d['url'], archive)
        got = sha256(archive)
        if got != d['sha256']:
            os.remove(archive)
            raise SystemExit('checksum mismatch for %s\n  expected %s\n  got      %s' % (d['url'], d['sha256'], got))
        tmp = os.path.join(dest, '.extract-' + d['name'])
        shutil.rmtree(tmp, ignore_errors=True)
        print('%-14s extracting' % d['name'])
        safe_extract(archive, tmp)
        os.rename(os.path.join(tmp, d['top']), out)
        shutil.rmtree(tmp)
        os.remove(archive)
    print('toolchain ready in', dest)


if __name__ == '__main__':
    main()
