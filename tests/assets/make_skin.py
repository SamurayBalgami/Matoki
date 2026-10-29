#!/usr/bin/env python3
# tests/assets/make_skin.py — 3 kemikli bükülen kol/tüp modeli ve animasyonlarını (GLB) üretir.
# Yalnız Python standart kütüphanesi kullanılır (DECISIONS D-063).
# Kullanım: python3 tests/assets/make_skin.py
import json, math, os, struct

OUT = os.path.dirname(os.path.abspath(__file__))

def pad4(b):
    return b + b'\x00' * ((4 - len(b) % 4) % 4)

def mat4_translate(tx, ty, tz):
    # Sütun-öncelikli (column-major) 4x4 öteleme matrisi
    return [
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        tx,  ty,  tz,  1.0,
    ]

def build_arm_glb():
    # 3 eklem:
    #  node 0 (eklem 0 - omuz):   yerel T=(0,0,0), dünya=(0,0,0), IBM = T(0, 0, 0)
    #  node 1 (eklem 1 - dirsek): yerel T=(0,1,0), dünya=(0,1,0), IBM = T(0,-1, 0)
    #  node 2 (eklem 2 - bilek):  yerel T=(0,1,0), dünya=(0,2,0), IBM = T(0,-2, 0)
    #  node 3: skinned mesh düğümü (mesh=0, skin=0)
    rings_y = [0.0, 0.5, 1.0, 1.5, 2.0]
    radius = 0.2
    cross = [(1.0, 0.0), (0.0, 1.0), (-1.0, 0.0), (0.0, -1.0)]

    pos, nrm, uv, joints, weights, indices = [], [], [], [], [], []
    for r_idx, y in enumerate(rings_y):
        # Ağırlık dağılımı:
        #  y=0.0 -> %100 eklem 0
        #  y=0.5 -> %50 eklem 0, %50 eklem 1
        #  y=1.0 -> %100 eklem 1 (dirsek)
        #  y=1.5 -> %50 eklem 1, %50 eklem 2
        #  y=2.0 -> %100 eklem 2 (uç/bilek)
        if r_idx == 0:
            j, w = (0, 0, 0, 0), (1.0, 0.0, 0.0, 0.0)
        elif r_idx == 1:
            j, w = (0, 1, 0, 0), (0.5, 0.5, 0.0, 0.0)
        elif r_idx == 2:
            j, w = (1, 0, 0, 0), (1.0, 0.0, 0.0, 0.0)
        elif r_idx == 3:
            j, w = (1, 2, 0, 0), (0.5, 0.5, 0.0, 0.0)
        else:
            j, w = (2, 0, 0, 0), (1.0, 0.0, 0.0, 0.0)

        for c_idx, (cx, cz) in enumerate(cross):
            pos.append((cx * radius, y, cz * radius))
            nrm.append((cx, 0.0, cz))
            uv.append((c_idx / 4.0, y / 2.0))
            joints.append(j)
            weights.append(w)

    # Uç kapak tepe noktası (köşe 20): tam (0, 2, 0) konumunda, normal (0, 1, 0), %100 eklem 2
    tip_index = len(pos)
    pos.append((0.0, 2.0, 0.0))
    nrm.append((0.0, 1.0, 0.0))
    uv.append((0.5, 1.0))
    joints.append((2, 0, 0, 0))
    weights.append((1.0, 0.0, 0.0, 0.0))

    # Tüp yan yüz üçgenleri
    for r_idx in range(len(rings_y) - 1):
        base0 = r_idx * 4
        base1 = (r_idx + 1) * 4
        for c_idx in range(4):
            n_idx = (c_idx + 1) % 4
            a = base0 + c_idx
            b = base0 + n_idx
            c = base1 + n_idx
            d = base1 + c_idx
            indices.extend([a, b, c, a, c, d])

    # Uç kapak üçgenleri (son halka -> tip_index)
    last_base = (len(rings_y) - 1) * 4
    for c_idx in range(4):
        n_idx = (c_idx + 1) % 4
        indices.extend([last_base + c_idx, last_base + n_idx, tip_index])

    bv, acc = [], []
    data = b''

    def add_accessor(blob, comp_type, count, val_type, mn=None, mx=None):
        nonlocal data
        data = pad4(data)
        view_idx = len(bv)
        bv.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(blob)})
        data += blob
        a = {
            "bufferView": view_idx,
            "byteOffset": 0,
            "componentType": comp_type,
            "count": count,
            "type": val_type,
        }
        if mn is not None:
            a["min"] = mn
        if mx is not None:
            a["max"] = mx
        acc.append(a)
        return len(acc) - 1

    v_cnt = len(pos)
    mn_p = [min(p[i] for p in pos) for i in range(3)]
    mx_p = [max(p[i] for p in pos) for i in range(3)]

    acc_pos = add_accessor(b''.join(struct.pack('<3f', *p) for p in pos), 5126, v_cnt, "VEC3", mn_p, mx_p)
    acc_nrm = add_accessor(b''.join(struct.pack('<3f', *n) for n in nrm), 5126, v_cnt, "VEC3")
    acc_uv  = add_accessor(b''.join(struct.pack('<2f', *u) for u in uv),  5126, v_cnt, "VEC2")
    acc_jnt = add_accessor(b''.join(struct.pack('<4H', *j) for j in joints), 5123, v_cnt, "VEC4")
    acc_wgt = add_accessor(b''.join(struct.pack('<4f', *w) for w in weights), 5126, v_cnt, "VEC4")
    acc_idx = add_accessor(struct.pack('<%dH' % len(indices), *indices), 5123, len(indices), "SCALAR")

    # inverseBindMatrices (3 adet MAT4)
    ibm_mats = (
        mat4_translate(0.0,  0.0, 0.0) +
        mat4_translate(0.0, -1.0, 0.0) +
        mat4_translate(0.0, -2.0, 0.0)
    )
    acc_ibm = add_accessor(struct.pack('<48f', *ibm_mats), 5126, 3, "MAT4")

    # Animasyon 0 ("bend_linear"): t = [0.0, 1.0, 2.0]
    # Dirsek (node 1) Z ekseni etrafında 0° -> +90° -> 0° döner (LINEAR)
    s45 = math.sin(math.pi / 4.0)
    c45 = math.cos(math.pi / 4.0)
    times_3 = [0.0, 1.0, 2.0]
    rot_linear = [
        (0.0, 0.0, 0.0, 1.0),
        (0.0, 0.0, s45, c45),
        (0.0, 0.0, 0.0, 1.0),
    ]
    acc_t3 = add_accessor(struct.pack('<3f', *times_3), 5126, 3, "SCALAR", [0.0], [2.0])
    acc_rot_lin = add_accessor(b''.join(struct.pack('<4f', *q) for q in rot_linear), 5126, 3, "VEC4")

    # Animasyon 1 ("step_pose"): STEP ile omuz (node 0) ötelemesi: t=[0, 1, 2] -> (0,0,0), (2,0,0), (4,0,0)
    trans_step = [
        (0.0, 0.0, 0.0),
        (2.0, 0.0, 0.0),
        (4.0, 0.0, 0.0),
    ]
    acc_tr_step = add_accessor(b''.join(struct.pack('<3f', *t) for t in trans_step), 5126, 3, "VEC3")

    # Animasyon 2 ("cubic_slide"): CUBICSPLINE ile omuz (node 0) ötelemesi: t=[0.0, 1.0]
    # k=0: in=(0,0,0), v0=(0,0,0), out=(0,2,0)
    # k=1: in=(0,2,0), v1=(1,1,0), out=(0,0,0)
    times_2 = [0.0, 1.0]
    cubic_vals = [
        (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (0.0, 2.0, 0.0),
        (0.0, 2.0, 0.0), (1.0, 1.0, 0.0), (0.0, 0.0, 0.0),
    ]
    acc_t2 = add_accessor(struct.pack('<2f', *times_2), 5126, 2, "SCALAR", [0.0], [1.0])
    acc_cubic = add_accessor(b''.join(struct.pack('<3f', *v) for v in cubic_vals), 5126, 6, "VEC3")

    doc = {
        "asset": {"version": "2.0", "generator": "matoki make_skin.py"},
        "scene": 0,
        "scenes": [{"nodes": [0, 3]}],
        "nodes": [
            {"name": "shoulder", "translation": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0], "children": [1]},
            {"name": "elbow",    "translation": [0.0, 1.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0], "children": [2]},
            {"name": "wrist",    "translation": [0.0, 1.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]},
            {"name": "arm_mesh", "mesh": 0, "skin": 0},
        ],
        "skins": [
            {"name": "arm_rig", "joints": [0, 1, 2], "skeleton": 0, "inverseBindMatrices": acc_ibm}
        ],
        "meshes": [
            {
                "name": "arm",
                "primitives": [
                    {
                        "attributes": {
                            "POSITION": acc_pos,
                            "NORMAL": acc_nrm,
                            "TEXCOORD_0": acc_uv,
                            "JOINTS_0": acc_jnt,
                            "WEIGHTS_0": acc_wgt,
                        },
                        "indices": acc_idx,
                        "material": 0,
                    }
                ],
            }
        ],
        "materials": [
            {"pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.7, 0.6, 1.0]}}
        ],
        "animations": [
            {
                "name": "bend_linear",
                "samplers": [
                    {"input": acc_t3, "output": acc_rot_lin, "interpolation": "LINEAR"}
                ],
                "channels": [
                    {"sampler": 0, "target": {"node": 1, "path": "rotation"}}
                ],
            },
            {
                "name": "step_pose",
                "samplers": [
                    {"input": acc_t3, "output": acc_tr_step, "interpolation": "STEP"}
                ],
                "channels": [
                    {"sampler": 0, "target": {"node": 0, "path": "translation"}}
                ],
            },
            {
                "name": "cubic_slide",
                "samplers": [
                    {"input": acc_t2, "output": acc_cubic, "interpolation": "CUBICSPLINE"}
                ],
                "channels": [
                    {"sampler": 0, "target": {"node": 0, "path": "translation"}}
                ],
            },
        ],
        "accessors": acc,
        "bufferViews": bv,
        "buffers": [{"byteLength": len(pad4(data))}],
    }

    js = pad4(json.dumps(doc).encode()).replace(b'\x00', b' ')
    binc = pad4(data)
    glb = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(binc))
    glb += struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(binc), 0x004E4942) + binc
    return glb

def main():
    glb = build_arm_glb()
    out_path = os.path.join(OUT, "arm_skin.glb")
    with open(out_path, "wb") as f:
        f.write(glb)
    print("uretildi: arm_skin.glb (%d bayt)" % len(glb))

if __name__ == '__main__':
    main()
