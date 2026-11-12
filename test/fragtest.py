"""fragtest.py [--vamos] SRC_IMAGE WORKDIR - fragmented writes, disk full and RENAME on a COPY.

Meant for a small, nearly full image with about 50 free 512-byte blocks:
  1. fill the free space with small files, delete every other one, then PUT a file that
     needs more than three extents (extents-overflow records); read it back, and check it
     with compare.py's independent overflow reader;
  2. PUT a file larger than the free space: it must fail and leave the volume as it was;
  3. RENAME: file rename, case-only rename, move into a folder, folder rename, folder
     move with contents, moving a folder into itself (refused), name clash (refused),
     volume rename;
  4. delete everything: listing, free space and CHECK back to the baseline.
The source image is only read.
"""
import os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ref')
VAMOS = '--vamos' in sys.argv
args = [a for a in sys.argv[1:] if not a.startswith('--')]
SRC, WORK = args[0], os.path.abspath(args[1])
IMG = os.path.join(WORK, 'f.hfv')
fails = 0


def wsl(p):
    p = os.path.abspath(p)
    return '/mnt/' + p[0].lower() + p[2:].replace('\\', '/')


def fail(msg):
    global fails
    fails += 1
    print('  FAIL:', msg)


def mb(*a, ok=True):
    def conv(x):
        if x.startswith(WORK):
            return ('W:' + os.path.relpath(x, WORK).replace('\\', '/')) if VAMOS else wsl(x)
        return x
    q = ' '.join("'%s'" % conv(x) for x in [IMG] + list(a))
    if VAMOS:
        cmd = '~/amitools-venv/bin/vamos -q -s 4 -V W:%s %s %s' % (wsl(WORK), wsl(os.path.join(ROOT, 'out', 'MacBrowse')), q)
    else:
        cmd = '%s %s' % (wsl(os.path.join(ROOT, 'out', 'macbrowse-host')), q)
    r = subprocess.run(['wsl', '-e', 'bash', '-lc', cmd], capture_output=True)
    out = r.stdout.decode('latin-1') + r.stderr.decode('latin-1')
    if ok and r.returncode:
        fail('command failed (%d): %s\n%s' % (r.returncode, ' '.join(a), out[-1500:]))
    return r.returncode, out


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


def free_blocks():
    return int(info()['Allocation block'].split(',')[2].split()[0])


def put(name, data, *extra, ok=True):
    p = os.path.join(WORK, name)
    open(p, 'wb').write(data)
    return mb('PUT', p, *extra, 'PLAIN', 'QUIET', ok=ok)


def get(path):
    o = os.path.join(WORK, 'back.bin')
    if os.path.exists(o): os.remove(o)
    mb('GET', path, 'TO', o, 'QUIET')
    return open(o, 'rb').read() if os.path.exists(o) else None


def main():
    if os.path.exists(WORK): shutil.rmtree(WORK)
    os.makedirs(WORK)
    shutil.copyfile(SRC, IMG)
    print('== work copy of', SRC, '(%s)' % ('vamos' if VAMOS else 'host'))
    base = check('baseline')
    vname = info()['Volume name']
    bs = int(info()['Allocation block'].split()[0])
    free0 = free_blocks()
    print('  %d free blocks of %d bytes' % (free0, bs))
    if free0 < 12: fail('image needs at least 12 free blocks'); return 1

    # 1. fragment the free space, then a file that needs > 3 extents
    hole = max(1, free0 // 12)
    n = free0 // hole
    for i in range(n):
        put('fill%02d' % i, bytes([i]) * (hole * bs))
    left = free_blocks()
    for i in range(0, n, 2):
        mb('DELETE', 'fill%02d' % i, 'QUIET')
    holes = free_blocks()
    big = os.urandom(min(holes, 4 * hole + 1) * bs - 100)
    put('fragmented', big)
    check('after fragmented PUT')
    if get('fragmented') != big: fail('fragmented file reads back wrong')
    else: print('  fragmented file (%d bytes in holes of %d blocks): read back ok' % (len(big), hole))
    r = subprocess.run([sys.executable, os.path.join(HERE, 'compare.py'), IMG], capture_output=True)
    out = r.stdout.decode('latin-1')
    if r.returncode or 'RESULT: OK' not in out: fail('compare.py: ' + out[-1500:])
    elif '0 of them fragmented' in out: fail('the file did not need overflow extents: ' + out[-300:])
    else: print('  compare.py (independent overflow reader): ok -', [l.strip() for l in out.splitlines() if 'forks:' in l][0])

    # 2. disk full: refused, nothing changed
    f1 = free_blocks()
    rc, out = put('toobig', os.urandom((f1 + 3) * bs), ok=False)
    if rc == 0: fail('PUT larger than the free space succeeded')
    if free_blocks() != f1: fail('disk-full PUT leaked blocks: %d -> %d' % (f1, free_blocks()))
    check('after disk-full PUT')
    print('  disk-full PUT refused cleanly: ok')

    # clean the fillers away for the rename tests
    mb('DELETE', 'fill#?', 'fragmented', 'QUIET')
    if free_blocks() != free0: fail('free space after cleanup %d, want %d' % (free_blocks(), free0))

    # 3. RENAME / MOVE
    put('alpha', b'A' * 300)
    mb('MKDIR', 'Dir1'); mb('MKDIR', 'Dir2'); mb('MKDIR', 'Dir1:Inner')
    put('inner', b'I' * 10, 'TO', 'Dir1:Inner')
    mb('RENAME', 'alpha', 'beta', 'QUIET')
    if get('beta') != b'A' * 300: fail('rename: beta')
    mb('RENAME', 'beta', 'BETA', 'QUIET')                   # case only
    if 'BETA' not in mb('LIST')[1]: fail('case-only rename')
    mb('RENAME', 'BETA', 'Dir2', 'QUIET')                   # move into a folder
    if get('Dir2:BETA') != b'A' * 300: fail('move into folder')
    mb('RENAME', 'Dir2:BETA', 'Dir1:Inner:gamma', 'QUIET')  # move + rename
    mb('RENAME', 'Dir1', 'Folder One', 'QUIET')             # folder rename (thread record)
    mb('RENAME', 'Folder One', 'Dir2', 'QUIET')             # folder move with contents
    if get('Dir2:Folder One:Inner:gamma') != b'A' * 300: fail('folder move: gamma')
    if get('Dir2:Folder One:Inner:inner') != b'I' * 10: fail('folder move: inner')
    rc, _ = mb('RENAME', 'Dir2', 'Dir2:Folder One:Inner', ok=False)
    if rc == 0: fail('moved a folder into its own subfolder')
    put('clash', b'x')
    rc, _ = mb('RENAME', 'clash', 'Dir2', ok=False)     # moves into Dir2: allowed
    rc, _ = mb('MKDIR', 'Dir3'); put('clash', b'y'); put('clash', b'z', 'TO', 'Dir3')
    rc, _ = mb('RENAME', 'clash', 'Dir3', ok=False)
    if rc == 0: fail('rename over an existing name succeeded')
    mb('RENAME', ':', 'Renamed Volume', 'QUIET')
    if info()['Volume name'] != 'Renamed Volume': fail('volume rename: %s' % info()['Volume name'])
    out = check('after renames')
    for want in ('Renamed Volume:Dir2:Folder One:Inner:gamma', 'Renamed Volume:Dir2:clash', 'Renamed Volume:Dir3:clash'):
        if want not in out: fail('hfsls does not show ' + want)
    print('  renames and moves: done')
    mb('RENAME', ':', vname, 'QUIET')

    # 4. back to the start
    mb('DELETE', 'Dir2:Folder One:Inner:#?', 'Dir2:Folder One:Inner', 'Dir2:Folder One', 'Dir2:#?', 'Dir3:#?', 'QUIET')
    mb('DELETE', 'Dir2', 'Dir3', 'clash', 'QUIET')
    end = check('after cleanup')
    if sorted(end.splitlines()) != sorted(base.splitlines()): fail('listing differs from the baseline')
    if free_blocks() != free0: fail('free space %d, want %d' % (free_blocks(), free0))
    print('RESULT:', 'OK' if not fails else '%d failure(s)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
