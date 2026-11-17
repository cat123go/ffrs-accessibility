# Holt die VP9-Videospur (IVF) aus einer unverschluesselten CRI-USM-Datei: usm2ivf.py <ein.usm> <aus.ivf>
import sys, struct
data = open(sys.argv[1], 'rb').read()
out, pos = bytearray(), 0
while pos + 8 <= len(data):
    sig = data[pos:pos + 4]
    size = struct.unpack('>I', data[pos + 4:pos + 8])[0]
    if sig == b'@SFV':
        header = data[pos + 9]
        padding = struct.unpack('>H', data[pos + 10:pos + 12])[0]
        kind = data[pos + 15] & 3
        if kind == 0:
            out += data[pos + 8 + header:pos + 8 + size - padding]
    pos += 8 + size
open(sys.argv[2], 'wb').write(out)
print(len(out), bytes(out[:4]))
