"""writetest.py SRC_IMAGE WORKDIR [--vamos] - exercise MacBrowse write support on a COPY of an image.

Copies SRC_IMAGE into WORKDIR, then: MKDIR, PUT hundreds of files (forcing B-tree splits),
PUT a folder tree, round-trips MacBinary / AppleSingle / AppleDouble / BinHex, DELETEs
everything again, and runs MacBrowse CHECK plus hfsls.py after every stage.  The original
image is never opened for writing.
"""
import os, shutil, struct, subprocess, sys, random, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ref')
VAMOS = '--vamos' in sys.argv
args = [a for a in sys.argv[1:] if not a.startswith('--')]
SRC, WORK = args[0], os.path.abspath(args[1])
IMG = os.path.join(WORK, 'w.hfv')
fails = 0


def wsl(p):
    p = os.path.abspath(p)
    return '/mnt/' + p[0].lower() + p[2:].replace('\\', '/')


def mb(*a, ok=True):
    """Run MacBrowse on the work image; paths under WORK are passed as W:..."""
    def conv(x):
        if isinstance(x, str) and x.startswith(WORK):
            rel = os.path.relpath(x, WORK).replace('\\', '/')
            return ('W:' + rel) if VAMOS else wsl(x)
        return x
    a = [conv(x) for x in a]
    img = 'W:w.hfv' if VAMOS else wsl(IMG)
    q = ' '.join("'%s'" % x for x in [img] + list(a))
    if VAMOS:
        cmd = '~/amitools-venv/bin/vamos -q -s 4 -V W:%s %s %s' % (wsl(WORK), wsl(os.path.join(ROOT, 'out', 'MacBrowse')), q)
    else:
        cmd = '%s %s' % (wsl(os.path.join(ROOT, 'out', 'macbrowse-host')), q)
    r = subprocess.run(['wsl', '-e', 'bash', '-lc', cmd], capture_output=True)
    out = r.stdout.decode('latin-1') + r.stderr.decode('latin-1')
    if ok and r.returncode:
        fail('command failed (%d): %s\n%s' % (r.returncode, ' '.join(a), out[-1500:]))
    return r.returncode, out


def fail(msg):
    global fails
    fails += 1
    print('  FAIL:', msg)


def check(stage):
    rc, out = mb('CHECK', ok=False)
    good = rc == 0 and 'Volume OK' in out
    print('  [%s] CHECK %s' % (stage, 'ok' if good else 'FAILED'))
    if not good: fail(out[-2000:])
    r = subprocess.run([sys.executable, os.path.join(REF, 'hfsls.py'), IMG, '99'], capture_output=True,
                       env=dict(os.environ, PYTHONIOENCODING='utf-8'))
    if r.returncode: fail('hfsls.py failed: ' + r.stderr.decode()[-500:])
    return r.stdout.decode('utf-8')


def info():
    _, out = mb('INFO')
    d = {}
    for l in out.splitlines():
        if ':' in l: k, v = l.split(':', 1); d[k.strip()] = v.strip()
    return d


def binhex(name, typ, cre, data, rsrc):
    """Minimal BinHex 4.0 encoder (no RLE) for testing the decoder."""
    def crc(b):
        c = 0
        for x in b + b'\0\0':
            for i in range(8):
                hi = c & 0x8000
                c = ((c << 1) | ((x >> (7 - i)) & 1)) & 0xffff
                if hi: c ^= 0x1021
        return struct.pack('>H', c)
    hdr = bytes([len(name)]) + name + b'\0' + typ + cre + b'\0\0' + struct.pack('>II', len(data), len(rsrc))
    raw = hdr + crc(hdr) + data + crc(data) + rsrc + crc(rsrc)
    raw = raw.replace(b'\x90', b'\x90\x00')
    chars = b'!"#$%&\'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr'
    bits = ''.join('{:08b}'.format(x) for x in raw)
    bits += '0' * (-len(bits) % 6)
    enc = bytes(chars[int(bits[i:i+6], 2)] for i in range(0, len(bits), 6))
    lines = [enc[i:i+64] for i in range(0, len(enc), 64)]
    return b'(This file must be converted with BinHex 4.0)\r\r:' + b'\r'.join(lines) + b':\r'


def main():
    if os.path.exists(WORK): shutil.rmtree(WORK)
    os.makedirs(WORK)
    shutil.copyfile(SRC, IMG)
    print('== work copy of', SRC, '(%s)' % ('vamos' if VAMOS else 'host'))
    base = check('baseline')
    free0 = info()['Allocation block']
    rng = random.Random(1)

    # 1. refusing to write a mounted volume without FORCE (if the copy is marked mounted)
    attr = info()['Attributes']
    force = []
    if 'MOUNTED' in attr:
        rc, out = mb('MKDIR', 'x', ok=False)
        if rc == 0: fail('wrote to a volume marked mounted without FORCE')
        else: print('  refused without FORCE: ok')
        force = ['FORCE']

    # 2. mkdir + many files -> B-tree splits
    mb('MKDIR', 'MBTest', *force)
    files = os.path.join(WORK, 'src'); os.makedirs(files)
    blobs = {}
    for i in range(150):
        n = 'file %03d %s.txt' % (i, 'x' * rng.randrange(0, 12))
        b = os.urandom(rng.choice([0, 1, 100, 511, 512, 513, 5000, 70000]))
        open(os.path.join(files, n), 'wb').write(b); blobs[n] = b
    mb('PUT', files, 'TO', 'MBTest', 'QUIET', *force)
    out = check('after PUT 150 files')
    for n in blobs:
        if 'MBTest:src:' + n not in out.replace('  ', ' ') and ':' + n not in out:
            fail('missing in hfsls: ' + n); break
    # read back a sample
    for n in rng.sample(sorted(blobs), 20):
        o = os.path.join(WORK, 'back.bin')
        mb('GET', 'MBTest:src:' + n, 'TO', o, 'QUIET')
        if open(o, 'rb').read() != blobs[n]: fail('data mismatch for ' + n)
    print('  read-back of 20 files: done')

    # 3. MacBinary / AppleSingle / AppleDouble / BinHex round trips of a file with both forks
    tree = check('pre-roundtrip')
    pick = None
    for l in base.splitlines():
        p = l[:60].rstrip()
        m = l[60:]
        if 'data=' in m and 'rsrc=' in m and len(p) < 60:
            dl = int(m.split('data=')[1].split()[0]); rl = int(m.split('rsrc=')[1])
            if 0 < dl < 3000000 and 0 < rl < 3000000 and all(32 <= ord(c) < 127 for c in p):
                pick = (p, dl, rl); break
    if pick:
        p = ':' + pick[0].split(':', 1)[1]
        name = p.rsplit(':', 1)[1]
        d0 = os.path.join(WORK, 'orig.data'); r0 = os.path.join(WORK, 'orig.rsrc')
        mb('GET', p, 'TO', d0, 'QUIET'); mb('GET', p, 'TO', r0, 'MODE', 'RSRC', 'QUIET')
        D, R = open(d0, 'rb').read(), open(r0, 'rb').read()
        for mode in ('MACBIN', 'SINGLE', 'DOUBLE'):
            ex = os.path.join(WORK, 'rt_' + mode.lower())
            mb('GET', p, 'TO', ex, 'MODE', mode, 'QUIET')
            src = ex + ('.bin' if mode == 'MACBIN' else '')
            mb('PUT', src, 'TO', 'MBTest', 'AS', 'rt ' + mode, *force)
            for fork, want in (('DATA', D), ('RSRC', R)):
                o = os.path.join(WORK, 'rt.out')
                mb('GET', 'MBTest:rt ' + mode, 'TO', o, 'MODE', fork, 'QUIET')
                if open(o, 'rb').read() != want: fail('%s round trip: %s fork differs' % (mode, fork))
            _, l1 = mb('LIST', p, 'QUIET'); _, l2 = mb('LIST', 'MBTest:rt ' + mode, 'QUIET')
            if l1.split()[-6:-2] != l2.split()[-6:-2]: fail('%s round trip: type/creator/sizes differ: %r vs %r' % (mode, l1, l2))
        print('  MacBinary/AppleSingle/AppleDouble round trips of %s: done' % name)
        hq = os.path.join(WORK, 'test.hqx')
        open(hq, 'wb').write(binhex(b'BinHex Test', b'TEXT', b'ttxt', D[:20000], R[:20000]))
        mb('PUT', hq, 'TO', 'MBTest', *force)
        o = os.path.join(WORK, 'hq.out')
        mb('GET', 'MBTest:BinHex Test', 'TO', o, 'QUIET')
        if open(o, 'rb').read() != D[:20000]: fail('BinHex data fork differs')
        mb('GET', 'MBTest:BinHex Test', 'TO', o, 'MODE', 'RSRC', 'QUIET')
        if open(o, 'rb').read() != R[:20000]: fail('BinHex rsrc fork differs')
        print('  BinHex decode: done')
    check('after round trips')

    # 4. REPLACE and duplicate refusal
    one = os.path.join(files, sorted(blobs)[0])
    rc, _ = mb('PUT', one, 'TO', 'MBTest:src', *force, ok=False)
    if rc == 0: fail('PUT over an existing name succeeded without REPLACE')
    mb('PUT', one, 'TO', 'MBTest:src', 'REPLACE', 'QUIET', *force)
    check('after REPLACE')

    # 5. delete everything again
    mb('DELETE', 'MBTest:src:#?', 'QUIET', *force)
    check('after deleting 150 files')
    mb('DELETE', 'MBTest:src', 'MBTest:#?', 'QUIET', *force)
    mb('DELETE', 'MBTest', *force)
    end = check('after deleting everything')
    if sorted(end.splitlines()) != sorted(base.splitlines()): fail('listing differs from baseline after cleanup')
    if info()['Allocation block'] != free0: fail('free space not restored: %s vs %s' % (info()['Allocation block'], free0))
    else: print('  free space restored: ok')
    print('RESULT:', 'OK' if not fails else '%d failure(s)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
