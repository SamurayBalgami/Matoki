#!/usr/bin/env python3
# tests/assets/make_font.py — Yalnız Python standart kütüphanesi ile minimal bir TrueType (.ttf)
# test yazı tipi üretir (DECISIONS D-063). İçerik:
#  - Glif 0 (.notdef), Glif 1 (' ' boş), Glif 2 ('A' üçgen), Glif 3 ('B' eksen hizalı kare),
#    Glif 4 ('O' delikli kontur), Glif 5 ('C' ikinci derece Bezier eğrisi),
#    Glif 6 ('D' ölçekli bileşik glif), Glif 7 ('Ğ' 2x2 matrisli bileşik glif),
#    Glif 8 ('ğ' cmap4 glyphIdArray testi), Glif 9 ('ı'/'ş'), Glif 10 (U+1F600 cmap12 + hmtx kuyruk lsb)
#  - cmap biçim 4 (hem idDelta hem idRangeOffset/glyphIdArray) + biçim 12
#  - kern biçim 0
import os, struct

OUT = os.path.dirname(os.path.abspath(__file__))

def pad4(b):
    return b + b'\x00' * ((4 - len(b) % 4) % 4)

def encode_simple_glyph(contours):
    if not contours:
        return b''
    end_pts = []
    pts = []
    idx = 0
    for c in contours:
        idx += len(c)
        end_pts.append(idx - 1)
        pts.extend(c)
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    xmin, ymin, xmax, ymax = min(xs), min(ys), max(xs), max(ys)

    flags = bytearray()
    x_bytes = bytearray()
    y_bytes = bytearray()
    px, py = 0, 0
    for (x, y, on_curve) in pts:
        dx = x - px
        dy = y - py
        px, py = x, y
        fl = 0x01 if on_curve else 0x00
        if dx == 0:
            fl |= 0x10
        elif -255 <= dx <= 255:
            fl |= 0x02 | (0x10 if dx > 0 else 0x00)
            x_bytes += struct.pack('>B', abs(dx))
        else:
            x_bytes += struct.pack('>h', dx)

        if dy == 0:
            fl |= 0x20
        elif -255 <= dy <= 255:
            fl |= 0x04 | (0x20 if dy > 0 else 0x00)
            y_bytes += struct.pack('>B', abs(dy))
        else:
            y_bytes += struct.pack('>h', dy)
        flags.append(fl)

    hdr = struct.pack('>hhhhh', len(contours), xmin, ymin, xmax, ymax)
    ends = struct.pack('>%dH' % len(end_pts), *end_pts)
    ins = struct.pack('>H', 0)
    return pad4(hdr + ends + ins + bytes(flags) + bytes(x_bytes) + bytes(y_bytes))

def f2dot14(val):
    return int(round(val * 16384.0))

def encode_composite_glyph(bbox, components):
    # components: [(child_gid, dx, dy, mode, mat_tuple)]
    # mode: 'none', 'scale', 'xy_scale', '2x2'
    hdr = struct.pack('>hhhhh', -1, *bbox)
    body = bytearray()
    for i, (gid, dx, dy, mode, mat) in enumerate(components):
        fl = 0x0001 | 0x0002 | 0x1000  # ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES | UNSCALED_COMPONENT_OFFSET
        if i + 1 < len(components):
            fl |= 0x0020  # MORE_COMPONENTS
        if mode == 'scale':
            fl |= 0x0008
        elif mode == 'xy_scale':
            fl |= 0x0040
        elif mode == '2x2':
            fl |= 0x0080
        body += struct.pack('>HHhh', fl, gid, dx, dy)
        if mode == 'scale':
            body += struct.pack('>h', f2dot14(mat[0]))
        elif mode == 'xy_scale':
            body += struct.pack('>hh', f2dot14(mat[0]), f2dot14(mat[1]))
        elif mode == '2x2':
            body += struct.pack('>hhhh', *(f2dot14(v) for v in mat))
    return pad4(hdr + bytes(body))

def build_cmap():
    # Eşlemeler:
    #  0x0020 ' ' -> 1
    #  0x0041 'A' -> 2
    #  0x0042 'B' -> 3
    #  0x0043 'C' -> 5
    #  0x0044 'D' -> 6
    #  0x004F 'O' -> 4
    #  0x011E 'Ğ' -> 7, 0x011F 'ğ' -> 8  (bu segment idRangeOffset != 0 kullanır)
    #  0x0131 'ı' -> 9
    #  0x015F 'ş' -> 9
    #  0x1F600    -> 10 (yalnız cmap 12)
    segs = [
        (0x0020, 0x0020, (1 - 0x0020) & 0xFFFF, None),
        (0x0041, 0x0042, (2 - 0x0041) & 0xFFFF, None),
        (0x0043, 0x0044, (5 - 0x0043) & 0xFFFF, None),
        (0x004F, 0x004F, (4 - 0x004F) & 0xFFFF, None),
        (0x011E, 0x011F, 0, [7, 8]),
        (0x0131, 0x0131, (9 - 0x0131) & 0xFFFF, None),
        (0x015F, 0x015F, (9 - 0x015F) & 0xFFFF, None),
        (0xFFFF, 0xFFFF, 1, None),
    ]
    seg_count = len(segs)
    end_counts = [s[1] for s in segs]
    start_counts = [s[0] for s in segs]
    id_deltas = [s[2] for s in segs]
    id_range_offs = []
    glyph_id_array = []
    for i, s in enumerate(segs):
        if s[3] is None:
            id_range_offs.append(0)
        else:
            # idRangeOffset[i] adresinden glyphIdArray'deki hedefe bayt uzaklığı
            roff = (seg_count - i) * 2 + len(glyph_id_array) * 2
            id_range_offs.append(roff)
            glyph_id_array.extend(s[3])

    sub4_body = (
        struct.pack('>%dH' % seg_count, *end_counts) +
        struct.pack('>H', 0) +
        struct.pack('>%dH' % seg_count, *start_counts) +
        struct.pack('>%dH' % seg_count, *id_deltas) +
        struct.pack('>%dH' % seg_count, *id_range_offs) +
        struct.pack('>%dH' % len(glyph_id_array), *glyph_id_array)
    )
    sub4_len = 14 + len(sub4_body)
    sub4 = struct.pack('>HHHHHHH', 4, sub4_len, 0, seg_count * 2, 16, 3, seg_count * 2 - 16) + sub4_body
    sub4 = pad4(sub4)

    groups = [
        (0x0020, 0x0020, 1),
        (0x0041, 0x0042, 2),
        (0x0043, 0x0044, 5),
        (0x004F, 0x004F, 4),
        (0x011E, 0x011F, 7),
        (0x0131, 0x0131, 9),
        (0x015F, 0x015F, 9),
        (0x1F600, 0x1F600, 10),
    ]
    sub12_len = 16 + len(groups) * 12
    sub12 = struct.pack('>HHIII', 12, 0, sub12_len, 0, len(groups))
    for g in groups:
        sub12 += struct.pack('>III', *g)

    # 2 encoding kaydı: (3, 1) -> sub4, (3, 10) -> sub12
    off4 = 4 + 2 * 8
    off12 = off4 + len(sub4)
    hdr = struct.pack('>HH', 0, 2)
    hdr += struct.pack('>HHI', 3, 1, off4)
    hdr += struct.pack('>HHI', 3, 10, off12)
    return hdr + sub4 + sub12

def build_kern():
    pairs = [
        (2, 3, -80),  # 'A','B'
        (2, 4, -50),  # 'A','O'
        (3, 2, -30),  # 'B','A'
        (4, 2, -40),  # 'O','A'
        (6, 7,  25),  # 'D','Ğ'
    ]
    n = len(pairs)
    sub_len = 14 + n * 6
    sub = struct.pack('>HHHHHHH', 0, sub_len, 0x0001, n, 24, 2, (n - 4) * 6)
    for l, r, v in pairs:
        sub += struct.pack('>HHh', l, r, v)
    return struct.pack('>HH', 0, 1) + sub

def build_ttf(long_loca=False):
    # Glifler (unitsPerEm = 1000):
    g0 = encode_simple_glyph([[(50, 50, 1), (50, 750, 1), (450, 750, 1), (450, 50, 1)]])
    g1 = b''  # ' ' boş glif
    # Glif 2: Üçgen (100,0)->(500,800)->(900,0), CW, alan = 320000
    g2 = encode_simple_glyph([[(100, 0, 1), (500, 800, 1), (900, 0, 1)]])
    # Glif 3: Eksen hizalı kare [100..700] x [100..700], CW, alan = 360000
    g3 = encode_simple_glyph([[(100, 100, 1), (100, 700, 1), (700, 700, 1), (700, 100, 1)]])
    # Glif 4: Delikli 'O' — dış CW [100..700]^2, iç CCW [300..500]^2, net alan = 320000
    g4 = encode_simple_glyph([
        [(100, 100, 1), (100, 700, 1), (700, 700, 1), (700, 100, 1)],
        [(300, 300, 1), (500, 300, 1), (500, 500, 1), (300, 500, 1)],
    ])
    # Glif 5: 4 ardışık off-curve kontrol noktalı Bezier eğrisi, analitik alan = 300000
    g5 = encode_simple_glyph([[(100, 100, 0), (100, 700, 0), (700, 700, 0), (700, 100, 0)]])
    # Glif 6: Bileşik glif (Glif 3 ölçek 0.5 + öteleme (50,50); Glif 2 xy-ölçek (0.5,0.5) + öteleme (400,100))
    g6 = encode_composite_glyph((100, 100, 850, 500), [
        (3, 50, 50, 'scale', (0.5,)),
        (2, 400, 100, 'xy_scale', (0.5, 0.5)),
    ])
    # Glif 7: 2x2 dönüşüm matrisli bileşik glif (a=0.5, b=0, c=0.25, d=0.5, det=0.25 -> alan=90000)
    g7 = encode_composite_glyph((100, 90, 605, 390), [
        (3, 80, 40, '2x2', (0.5, 0.0, 0.25, 0.5)),
    ])
    # Glif 8, 9, 10: basit kutular
    g8  = encode_simple_glyph([[(60, 0, 1), (60, 500, 1), (560, 500, 1), (560, 0, 1)]])
    g9  = encode_simple_glyph([[(50, 0, 1), (50, 500, 1), (250, 500, 1), (250, 0, 1)]])
    g10 = encode_simple_glyph([[(45, 0, 1), (45, 600, 1), (545, 600, 1), (545, 0, 1)]])

    glyphs = [g0, g1, g2, g3, g4, g5, g6, g7, g8, g9, g10]
    num_glyphs = len(glyphs)

    glyf = b''
    offs = [0]
    for g in glyphs:
        glyf += g
        offs.append(len(glyf))

    if long_loca:
        loca = struct.pack('>%dI' % len(offs), *offs)
    else:
        loca = struct.pack('>%dH' % len(offs), *(o // 2 for o in offs))

    # head (54 bayt)
    head = struct.pack(
        '>IIIIHHqqhhhhHHhhh',
        0x00010000, 0x00010000, 0, 0x5F0F3CF5,
        0x000B, 1000, 0, 0,
        0, -200, 1000, 800,
        0, 8, 2, 1 if long_loca else 0, 0
    )

    # hhea (36 bayt): numberOfHMetrics = 10 (11. glif kuyruk lsb dizisini test eder)
    num_hm = 10
    hhea = struct.pack(
        '>IhhhHhhhhhhhhhhhH',
        0x00010000, 800, -200, 100,
        1000, 0, 0, 900,
        1, 0, 0, 0, 0, 0, 0, 0, num_hm
    )

    # hmtx: 10 adet (advance, lsb) + 1 adet kuyruk lsb
    hmetrics = [
        (500, 50),   # 0: .notdef
        (300, 0),    # 1: ' '
        (1000, 100), # 2: 'A'
        (800, 100),  # 3: 'B'
        (800, 100),  # 4: 'O'
        (700, 100),  # 5: 'C'
        (950, 100),  # 6: 'D'
        (850, 80),   # 7: 'Ğ'
        (750, 60),   # 8: 'ğ'
        (650, 50),   # 9: 'ı'/'ş'
    ]
    hmtx = b''.join(struct.pack('>Hh', adv, lsb) for adv, lsb in hmetrics)
    hmtx += struct.pack('>h', 45)  # 10: U+1F600 (advance=650'yi glif 9'dan miras alır, lsb=45)

    # maxp (32 bayt, sürüm 1.0)
    maxp = struct.pack('>IH', 0x00010000, num_glyphs) + b'\x00' * 26

    tables = {
        b'cmap': build_cmap(),
        b'glyf': glyf,
        b'head': head,
        b'hhea': hhea,
        b'hmtx': hmtx,
        b'kern': build_kern(),
        b'loca': loca,
        b'maxp': maxp,
    }

    nt = len(tables)
    out = struct.pack('>IHHHH', 0x00010000, nt, 128, 3, nt * 16 - 128)
    data_off = 12 + nt * 16
    body = b''
    for tag in sorted(tables.keys()):
        raw = tables[tag]
        out += struct.pack('>4sIII', tag, 0, data_off + len(body), len(raw))
        body += pad4(raw)
    return out + body

def main():
    ttf = build_ttf(long_loca=False)
    out_path = os.path.join(OUT, 'test_font.ttf')
    with open(out_path, 'wb') as f:
        f.write(ttf)
    print('uretildi: test_font.ttf (%d bayt)' % len(ttf))

if __name__ == '__main__':
    main()
