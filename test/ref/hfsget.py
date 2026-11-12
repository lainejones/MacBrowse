"""hfsget.py IMAGE NAME OUT [rsrc] - copy one file's data (or resource) fork out of a bare HFS image.
NAME is matched against the file name only (first match). Only the catalog's first three
extents are read, so heavily fragmented files may come out short (a warning is printed)."""
import struct, sys
img, want, out = sys.argv[1], sys.argv[2], sys.argv[3]
rsrc = len(sys.argv) > 4 and sys.argv[4] == 'rsrc'
f = open(img, 'rb')
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
for n in range(len(cat) // nodesize):
    node = cat[n*nodesize:(n+1)*nodesize]
    if node[8] != 0xff: continue
    nrec = struct.unpack('>H', node[10:12])[0]
    for r in range(nrec):
        off = struct.unpack('>H', node[nodesize-2-2*r:nodesize-2*r])[0]
        klen = node[off]
        if klen == 0: continue
        nl = node[off+6]; name = node[off+7:off+7+nl].decode('mac_roman')
        d = off + 1 + klen; d += d & 1
        if node[d] != 2 or name != want: continue
        if rsrc:
            length = struct.unpack('>I', node[d+36:d+40])[0]; eo = d + 86
        else:
            length = struct.unpack('>I', node[d+26:d+30])[0]; eo = d + 74
        buf = b''
        for i in range(3):
            st, cnt = struct.unpack('>HH', node[eo+4*i:eo+4*i+4])
            if cnt:
                f.seek(alblst * 512 + st * alblksiz); buf += f.read(cnt * alblksiz)
        if len(buf) < length:
            print('warning: file has extents beyond the catalog record; output truncated', file=sys.stderr)
        open(out, 'wb').write(buf[:length])
        print('wrote', out, min(len(buf), length), 'of', length)
        sys.exit(0)
sys.exit('not found: ' + want)
