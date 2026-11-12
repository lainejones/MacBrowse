"""hfsls.py IMAGE [DEPTH] - list a bare HFS volume image (type/creator, data+resource sizes)."""
import struct, sys
p = sys.argv[1]; maxdepth = int(sys.argv[2]) if len(sys.argv) > 2 else 3
f = open(p, 'rb')
f.seek(1024); mdb = f.read(512)
assert mdb[:2] == b'BD', 'not HFS'
alblksiz = struct.unpack('>I', mdb[20:24])[0]
alblst = struct.unpack('>H', mdb[28:30])[0]
ctsize = struct.unpack('>I', mdb[146:150])[0]
ext = [struct.unpack('>HH', mdb[150+4*i:154+4*i]) for i in range(3)]
cat = b''
for st, cnt in ext:
    if cnt:
        f.seek(alblst * 512 + st * alblksiz); cat += f.read(cnt * alblksiz)
cat = cat[:ctsize]
nodesize = struct.unpack('>H', cat[14+18:14+20])[0]
dirs = {1: ('', 0)}; files = []
for n in range(len(cat) // nodesize):
    node = cat[n*nodesize:(n+1)*nodesize]
    if node[8] != 0xff: continue
    nrec = struct.unpack('>H', node[10:12])[0]
    for r in range(nrec):
        off = struct.unpack('>H', node[nodesize-2-2*r:nodesize-2*r])[0]
        klen = node[off]
        if klen == 0: continue
        par = struct.unpack('>I', node[off+2:off+6])[0]
        nl = node[off+6]; name = node[off+7:off+7+nl].decode('mac_roman')
        d = off + 1 + klen; d += d & 1
        t = node[d]
        if t == 1:
            did = struct.unpack('>I', node[d+6:d+10])[0]; dirs[did] = (name, par)
        elif t == 2:
            ftype = node[d+4:d+8].decode('mac_roman'); fcre = node[d+8:d+12].decode('mac_roman')
            dl = struct.unpack('>I', node[d+26:d+30])[0]; rl = struct.unpack('>I', node[d+36:d+40])[0]
            files.append((par, name, ftype, fcre, dl, rl))
def path(did):
    out = []
    while did in dirs and did != 1:
        n, par = dirs[did]; out.append(n); did = par
    return ':'.join(reversed(out))
for par, name, t, c, dl, rl in sorted(files, key=lambda x: (path(x[0]), x[1])):
    pp = path(par)
    if pp.count(':') >= maxdepth: continue
    print('%-60s %s/%s data=%d rsrc=%d' % ((pp + ':' + name)[:60], t, c, dl, rl))
