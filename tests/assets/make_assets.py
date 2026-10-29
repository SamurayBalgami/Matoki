#!/usr/bin/env python3
# tests/assets/make_assets.py — glTF/GLB test varlıklarını üretir (yalnız standart kütüphane).
# Dış kaynaklı model dosyası repoya girmez (DECISIONS D-063); bu betik çıktıları yeniden üretir.
# Kullanım: python3 tests/assets/make_assets.py  (çıktılar tests/assets/ altına yazılır)
import base64, json, math, os, struct, zlib

OUT = os.path.dirname(os.path.abspath(__file__))

def png_rgba(w, h, pixels):
    raw = b''.join(b'\x00' + bytes(pixels[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

def checker(w, h):
    px = []
    for y in range(h):
        for x in range(w):
            c = (255, 40, 40, 255) if (x + y) % 2 == 0 else (40, 40, 255, 255)
            px.extend(c)
    return px

# Küp: yüz başına 4 köşe, CCW dışarıdan
FACES = [((1,0,0),(0,0,-1),(0,1,0)), ((-1,0,0),(0,0,1),(0,1,0)), ((0,1,0),(1,0,0),(0,0,-1)),
         ((0,-1,0),(1,0,0),(0,0,1)), ((0,0,1),(1,0,0),(0,1,0)), ((0,0,-1),(-1,0,0),(0,1,0))]
def cube():
    pos, nrm, uv, idx = [], [], [], []
    for f, (n, s, t) in enumerate(FACES):
        for (a, b) in [(-1,-1),(1,-1),(1,1),(-1,1)]:
            pos.append(tuple(0.5 * (n[i] + a * s[i] + b * t[i]) for i in range(3)))
            nrm.append(n)
            uv.append((a * 0.5 + 0.5, 0.5 - b * 0.5))
        k = f * 4
        idx += [k, k+1, k+2, k, k+2, k+3]
    return pos, nrm, uv, idx

def pad4(b):
    return b + b'\x00' * ((4 - len(b) % 4) % 4)

def build(variant):
    pos, nrm, uv, idx = cube()
    n = len(pos)
    bv, acc = [], []
    data = b''
    def add_view(blob, stride=None, target=None):
        nonlocal data
        data = pad4(data)
        v = {"buffer": 0, "byteOffset": len(data), "byteLength": len(blob)}
        if stride: v["byteStride"] = stride
        if target: v["target"] = target
        data += blob
        bv.append(v)
        return len(bv) - 1
    mn = [min(p[i] for p in pos) for i in range(3)]
    mx = [max(p[i] for p in pos) for i in range(3)]
    if variant == 'gltf':
        # iç içe (interleaved): pos(12) nrm(12) uv(8) = 32 bayt adım
        blob = b''.join(struct.pack('<3f3f2f', *pos[i], *nrm[i], *uv[i]) for i in range(n))
        v = add_view(blob, 32, 34962)
        acc.append({"bufferView": v, "byteOffset": 0, "componentType": 5126, "count": n, "type": "VEC3", "min": mn, "max": mx})
        acc.append({"bufferView": v, "byteOffset": 12, "componentType": 5126, "count": n, "type": "VEC3"})
        acc.append({"bufferView": v, "byteOffset": 24, "componentType": 5126, "count": n, "type": "VEC2"})
        iv = add_view(struct.pack('<%dH' % len(idx), *idx), None, 34963)
        acc.append({"bufferView": iv, "componentType": 5123, "count": len(idx), "type": "SCALAR"})
    elif variant == 'glb':
        v0 = add_view(b''.join(struct.pack('<3f', *p) for p in pos))
        acc.append({"bufferView": v0, "componentType": 5126, "count": n, "type": "VEC3", "min": mn, "max": mx})
        v1 = add_view(b''.join(struct.pack('<3f', *p) for p in nrm))
        acc.append({"bufferView": v1, "componentType": 5126, "count": n, "type": "VEC3"})
        # normalized u16 UV
        v2 = add_view(b''.join(struct.pack('<2H', round(u * 65535), round(t * 65535)) for (u, t) in uv))
        acc.append({"bufferView": v2, "componentType": 5123, "normalized": True, "count": n, "type": "VEC2"})
        v3 = add_view(struct.pack('<%dI' % len(idx), *idx))
        acc.append({"bufferView": v3, "componentType": 5125, "count": len(idx), "type": "SCALAR"})
    else:  # 'u8': u8 indeks + normalized u8 UV, normal yok (yükleyici üretir)
        v0 = add_view(b''.join(struct.pack('<3f', *p) for p in pos))
        acc.append({"bufferView": v0, "componentType": 5126, "count": n, "type": "VEC3", "min": mn, "max": mx})
        acc.append(None)
        v2 = add_view(b''.join(struct.pack('<2B', round(u * 255), round(t * 255)) for (u, t) in uv) )
        acc.append({"bufferView": v2, "componentType": 5121, "normalized": True, "count": n, "type": "VEC2"})
        v3 = add_view(struct.pack('<%dB' % len(idx), *idx))
        acc.append({"bufferView": v3, "componentType": 5121, "count": len(idx), "type": "SCALAR"})

    png = png_rgba(4, 4, checker(4, 4))
    attrs = {"POSITION": 0, "TEXCOORD_0": 2}
    if acc[1] is not None: attrs["NORMAL"] = 1
    else: acc[1] = acc[0]   # yer tutucu (kullanılmaz)
    doc = {
        "asset": {"version": "2.0", "generator": "matoki make_assets.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "meshes": [{"primitives": [{"attributes": attrs, "indices": 3, "material": 0}]}],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.5, 0.25, 1.0], "baseColorTexture": {"index": 0}}}],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"wrapS": 33071, "wrapT": 33071}],
        "accessors": acc,
        "bufferViews": bv,
    }
    q = math.sin(math.pi / 4)
    if variant == 'glb':
        # kök node matrix ile: x'te +3 öteleme; çocuk ölçek 2
        doc["nodes"] = [{"matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 3,0,0,1], "children": [1]}, {"mesh": 0, "scale": [2, 2, 2]}]
        data = pad4(data)
        img_view = len(bv)
        data += png
        bv.append({"buffer": 0, "byteOffset": len(data) - len(png), "byteLength": len(png)})
        doc["images"] = [{"bufferView": img_view, "mimeType": "image/png"}]
        doc["buffers"] = [{"byteLength": len(pad4(data))}]
        js = pad4(json.dumps(doc).encode()).replace(b'\x00', b' ')
        binc = pad4(data)
        glb = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(binc))
        glb += struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(binc), 0x004E4942) + binc
        return glb
    # kök: y'de +1 öteleme ve Y etrafında 90°; çocuk: ölçek 2 + mesh
    doc["nodes"] = [{"translation": [0, 1, 0], "rotation": [0, q, 0, q], "children": [1]}, {"mesh": 0, "scale": [2, 2, 2]}]
    doc["images"] = [{"uri": "data:image/png;base64," + base64.b64encode(png).decode()}]
    doc["buffers"] = [{"byteLength": len(data), "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode()}]
    return json.dumps(doc, indent=1).encode()

def main():
    outs = {"cube_embedded.gltf": build('gltf'), "cube_binary.glb": build('glb'), "cube_u8.gltf": build('u8')}
    for name, blob in outs.items():
        with open(os.path.join(OUT, name), 'wb') as f:
            f.write(blob)
    # Bozuk varyantlar
    glb = outs["cube_binary.glb"]
    with open(os.path.join(OUT, "bad_truncated.glb"), 'wb') as f:
        f.write(glb[:len(glb) // 2])
    doc = json.loads(outs["cube_embedded.gltf"])
    doc["accessors"][3]["count"] = 100000   # bufferView'den taşan accessor
    with open(os.path.join(OUT, "bad_accessor.gltf"), 'w') as f:
        json.dump(doc, f)
    doc = json.loads(outs["cube_embedded.gltf"])
    doc["asset"]["version"] = "1.0"
    with open(os.path.join(OUT, "bad_version.gltf"), 'w') as f:
        json.dump(doc, f)
    print("uretildi:", ", ".join(sorted(list(outs) + ["bad_truncated.glb", "bad_accessor.gltf", "bad_version.gltf"])))

if __name__ == '__main__':
    main()
