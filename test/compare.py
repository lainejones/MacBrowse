"""compare.py IMAGE... - compare `MacBrowse IMAGE LS RAW` with hfsls.py, and extracted forks with hfsget.py.

Runs the host build (out/macbrowse-host, via WSL) or, with --vamos, the Amiga binary under vamos.
Listing: lines are compared as sets (hfsls sorts by path; MacBrowse prints catalog order).
Forks: every file whose name is unique on the volume and whose fork fits in the first three
extents (what hfsget.py can read) is extracted by both tools and compared byte for byte.
"""
import os, struct, subprocess, sys, tempfile, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ref')
VAMOS = '~/amitools-venv/bin/vamos'


def wsl_path(p):
    p = os.path.abspath(p)
    return '/mnt/' + p[0].lower() + p[2:].replace('\\', '/')


def run_mb(args, vamos=False, img=None):
    if vamos:
        # map the image folder as volume IMG:, scratch as T:
        d = os.path.dirname(os.path.abspath(img))
        cmd = '%s -q -s 4 -V IMG:%s -V OUT:%s %s %s' % (
            VAMOS, wsl_path(d), wsl_path(tempfile.gettempdir()), wsl_path(os.path.join(ROOT, 'out', 'MacBrowse')),
            ' '.join("'%s'" % a for a in args))
    else:
        cmd = '%s %s' % (wsl_path(os.path.join(ROOT, 'out', 'macbrowse-host')), ' '.join("'%s'" % a for a in args))
    return subprocess.run(['wsl', '-e', 'bash', '-lc', cmd], capture_output=True)


def catalog_files(img):
    """Return [(name, dlen, rlen, d_in_3ext, r_in_3ext)] using the same logic as hfsls.py."""
    f = open(img, 'rb'); f.seek(1024); mdb = f.read(512)
    alblksiz = struct.unpack('>I', mdb[20:24])[0]; alblst = struct.unpack('>H', mdb[28:30])[0]
    ctsize = struct.unpack('>I', mdb[146:150])[0]
    ext = [struct.unpack('>HH', mdb[150+4*i:154+4*i]) for i in range(3)]
    cat = b''
    for st, cnt in ext:
        if cnt: f.seek(alblst*512 + st*alblksiz); cat += f.read(cnt*alblksiz)
    cat = cat[:ctsize]; ns = struct.unpack('>H', cat[32:34])[0]; out = []; dirs = {}
    for n in range(len(cat)//ns):
        node = cat[n*ns:(n+1)*ns]
        if node[8] != 0xff: continue
        for r in range(struct.unpack('>H', node[10:12])[0]):
            off = struct.unpack('>H', node[ns-2-2*r:ns-2*r])[0]; kl = node[off]
            if not kl: continue
            nl = node[off+6]; name = node[off+7:off+7+nl]
            d = off+1+kl; d += d & 1
            par = struct.unpack('>I', node[off+2:off+6])[0]
            if node[d] == 1: dirs[struct.unpack('>I', node[d+6:d+10])[0]] = (name, par)
            if node[d] != 2: continue
            dl = struct.unpack('>I', node[d+26:d+30])[0]; rl = struct.unpack('>I', node[d+36:d+40])[0]
            def cov(eo):
                return sum(struct.unpack('>HH', node[eo+4*i:eo+4*i+4])[1] for i in range(3)) * alblksiz
            out.append((name, dl, rl, cov(d+74) >= dl, cov(d+86) >= rl, par))
    def path(did):
        o = []
        while did in dirs and did != 2:
            n, did = dirs[did]; o.append(n)
        return b''.join(x + b':' for x in reversed(o))
    return [(n, dl, rl, a, b, b':' + path(par) + n) for n, dl, rl, a, b, par in out]


def main():
    vamos = '--vamos' in sys.argv
    imgs = [a for a in sys.argv[1:] if not a.startswith('--')]
    maxforks = 40
    bad = 0
    for img in imgs:
        print('==', img)
        ref = subprocess.run([sys.executable, os.path.join(REF, 'hfsls.py'), img, '99'], capture_output=True,
                             env=dict(os.environ, PYTHONIOENCODING='utf-8'))
        refl = set(ref.stdout.decode('utf-8').replace('\r\n', '\n').splitlines())
        mbimg = ('IMG:' + os.path.basename(img)) if vamos else wsl_path(img)
        mb = run_mb([mbimg, 'LS', 'RAW'], vamos, img)
        if mb.returncode:
            print('  MacBrowse failed:', mb.stderr.decode('latin-1')); bad += 1; continue
        mbl = set(mb.stdout.decode('mac_roman').splitlines())
        only_ref, only_mb = refl - mbl, mbl - refl
        print('  listing: hfsls %d lines, MacBrowse %d lines, only-hfsls %d, only-MacBrowse %d'
              % (len(refl), len(mbl), len(only_ref), len(only_mb)))
        for l in sorted(only_ref)[:10]: print('   - hfsls only:', l)
        for l in sorted(only_mb)[:10]: print('   + MacBrowse only:', l)
        if only_ref: bad += 1
        # forks
        files = catalog_files(img)
        names = {}
        for fl in files: names[fl[0]] = names.get(fl[0], 0) + 1
        cand = [fl for fl in files if names[fl[0]] == 1 and (fl[1] or fl[2])]
        # spread the sample over the volume: biggest files first, then the rest
        cand.sort(key=lambda x: -(x[1] + x[2]))
        cand = cand[:maxforks]
        nok = nskip = 0
        tmp = tempfile.mkdtemp()
        for name, dl, rl, din, rin, path in cand:
            sname = name.decode('mac_roman')
            for fork, ln, inn in (('data', dl, din), ('rsrc', rl, rin)):
                if not ln: continue
                rp = os.path.join(tmp, 'ref.bin')
                if not inn:
                    nskip += 1    # hfsget.py would truncate: use the independent overflow reader
                    open(rp, 'wb').write(read_fragmented(img, name, fork))
                else:
                    args = [sys.executable, os.path.join(REF, 'hfsget.py'), img, sname, rp] + (['rsrc'] if fork == 'rsrc' else [])
                    subprocess.run(args, capture_output=True)
                pat = '#?:' if False else ''
                outname = 'mb.bin'
                if vamos:
                    dest = 'OUT:' + os.path.basename(tmp) + '/' + outname
                else:
                    dest = wsl_path(os.path.join(tmp, outname))
                # find the file by name anywhere: use LS output path
                r = run_mb([mbimg, 'GET', ''.join(chr(c) if 32 <= c < 127 and c != 39 else '?' for c in path), 'TO', dest, 'RAW', 'QUIET'] + (['MODE', 'RSRC'] if fork == 'rsrc' else []), vamos, img)
                mp = os.path.join(tmp, outname)
                a = open(rp, 'rb').read() if os.path.exists(rp) else None
                b = open(mp, 'rb').read() if os.path.exists(mp) else None
                if a is None or b is None or a != b:
                    print('   FORK MISMATCH %s %s ref=%s mb=%s %s' % (sname, fork, a and len(a), b and len(b), r.stderr[:200]))
                    bad += 1
                else:
                    nok += 1
                for p in (rp, mp):
                    if os.path.exists(p): os.remove(p)
        print('  forks: %d identical (%d of them fragmented beyond 3 extents, checked with the overflow reader in this script)' % (nok, nskip))
    print('RESULT:', 'OK' if not bad else '%d problem(s)' % bad)
    return 1 if bad else 0


def read_fragmented(img, want, fork):
    """Read a fork including extents-overflow records (independent of MacBrowse)."""
    f = open(img, 'rb'); f.seek(1024); mdb = f.read(512)
    abs_ = struct.unpack('>I', mdb[20:24])[0]; ast = struct.unpack('>H', mdb[28:30])[0]
    def readext(exts):
        b = b''
        for st, cnt in exts:
            if cnt: f.seek(ast*512 + st*abs_); b += f.read(cnt*abs_)
        return b
    def ext3(b): return [struct.unpack('>HH', b[4*i:4*i+4]) for i in range(3)]
    xt = readext(ext3(mdb[134:146]))[:struct.unpack('>I', mdb[130:134])[0]]
    over = {}
    for n in range(len(xt)//512):
        node = xt[n*512:(n+1)*512]
        if node[8] != 0xff: continue
        for r in range(struct.unpack('>H', node[10:12])[0]):
            off = struct.unpack('>H', node[510-2*r:512-2*r])[0]
            ft, fid, sb = node[off+1], struct.unpack('>I', node[off+2:off+6])[0], struct.unpack('>H', node[off+6:off+8])[0]
            over[(ft, fid, sb)] = ext3(node[off+8:off+20])
    cat = readext(ext3(mdb[150:162]))   # catalog itself assumed unfragmented here
    for n in range(len(cat)//512):
        node = cat[n*512:(n+1)*512]
        if node[8] != 0xff: continue
        for r in range(struct.unpack('>H', node[10:12])[0]):
            off = struct.unpack('>H', node[510-2*r:512-2*r])[0]; kl = node[off]
            if not kl or node[off+7:off+7+node[off+6]] != want: continue
            d = off+1+kl; d += d & 1
            if node[d] != 2: continue
            fid = struct.unpack('>I', node[d+20:d+24])[0]
            ln = struct.unpack('>I', node[d+(36 if fork == 'rsrc' else 26):][:4])[0]
            exts = ext3(node[d+(86 if fork == 'rsrc' else 74):][:12])
            ft = 0xff if fork == 'rsrc' else 0
            while sum(c for _, c in exts) * abs_ < ln:
                exts += over[(ft, fid, sum(c for _, c in exts))]
            return readext(exts)[:ln]
    raise SystemExit('not found')


def find_path(lsout, name):
    for line in lsout.split(b'\n'):
        p = line[:60].rstrip(b' ')
        # LS truncates at 60; only use untruncated lines
        if p.endswith(b':' + name) and len(line) > 61 and line[60:61] == b' ' and len(p) < 60:
            # drop the volume name component
            return b':' + p.split(b':', 1)[1]
    raise SystemExit('path not found for %r' % name)


if __name__ == '__main__':
    sys.exit(main())
