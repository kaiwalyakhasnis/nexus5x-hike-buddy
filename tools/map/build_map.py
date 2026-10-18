#!/usr/bin/env python3
"""Builds hike-buddy's offline map from an OpenStreetMap extract.

Keeps what matters on a hike -- trails, tracks, roads, railways, rivers,
lakes, coast, peaks, huts, springs, stations -- for one bounding box, and
writes it as tiles at three levels of detail (see hike-buddy/map.h for the
format). Map data (c) OpenStreetMap contributors, ODbL.

  python build_map.py kanto.osm.pbf hike-buddy.map [west south east north]

Needs pyosmium and shapely (pip install osmium shapely).
"""
import math
import struct
import sys

import osmium
from shapely.geometry import LineString
from shapely import clip_by_rect

# Tokyo (without the islands) and Kanagawa.
BBOX = (138.90, 35.12, 139.95, 35.90)

WORLD_BITS = 28  # global coordinates: Web Mercator, 2^28 units around the world

# Line kinds (hike-buddy/map.h has the same numbers).
MOTORWAY, TRUNK, PRIMARY, SECONDARY, TERTIARY, MINOR, TRACK, TRAIL, RAIL = 1, 2, 3, 4, 5, 6, 7, 8, 9
RIVER, STREAM, COAST, LAKE = 10, 11, 12, 13
# Point kinds.
PEAK, HUT, WATER, STATION = 20, 21, 22, 23

HIGHWAY = {
    'motorway': MOTORWAY, 'motorway_link': MOTORWAY,
    'trunk': TRUNK, 'trunk_link': TRUNK,
    'primary': PRIMARY, 'primary_link': PRIMARY,
    'secondary': SECONDARY, 'secondary_link': SECONDARY,
    'tertiary': TERTIARY, 'tertiary_link': TERTIARY,
    # Residential streets are two thirds of all ways in Tokyo: left out to
    # keep the map small enough for a RAM-booted ramdisk.
    'track': TRACK,
    'path': TRAIL, 'bridleway': TRAIL,
}

# Levels: tile zoom, kinds kept, simplification tolerance in metres.
LEVELS = [
    (13, None, 1.5),  # detail: everything
    (11, {MOTORWAY, TRUNK, PRIMARY, SECONDARY, TERTIARY, TRACK, TRAIL, RAIL, RIVER, COAST, LAKE,
          PEAK, HUT, STATION}, 6.0),
    (9, {MOTORWAY, TRUNK, PRIMARY, RAIL, COAST, LAKE, PEAK}, 25.0),
]

# Drawing order inside a tile: water at the bottom, trails and points on top.
ORDER = [LAKE, COAST, STREAM, RIVER, MINOR, TRACK, TERTIARY, SECONDARY, PRIMARY, TRUNK, MOTORWAY,
         RAIL, TRAIL, WATER, HUT, STATION, PEAK]


def project(lon, lat):
    """lon/lat -> global units (float)."""
    w = 1 << WORLD_BITS
    x = (lon + 180.0) / 360.0 * w
    s = math.sin(math.radians(lat))
    y = (0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * w
    return x, y


def units_per_metre(lat):
    return (1 << WORLD_BITS) / (40075016.686 * math.cos(math.radians(lat)))


def latin_name(tags):
    for k in ('name:en', 'name:ja-Latn', 'name:ja_rm', 'name:ja-Latn-hepburn'):
        v = tags.get(k)
        if v:
            try:
                v.encode('ascii')
            except UnicodeEncodeError:
                continue
            return v[:40]
    return ''


def parse_ele(v):
    if not v:
        return 0
    try:
        return max(0, min(9999, int(round(float(v.replace('m', '').strip())))))
    except ValueError:
        return 0


def classify_way(tags):
    hw = tags.get('highway')
    if hw:
        k = HIGHWAY.get(hw)
        if k:
            return k
        # City footways are endless; keep only the ones that look like trails.
        if hw in ('footway', 'steps') and ('sac_scale' in tags or 'trail_visibility' in tags):
            return TRAIL
        return None
    rw = tags.get('railway')
    if rw in ('rail', 'narrow_gauge', 'light_rail', 'funicular') and tags.get('tunnel') != 'yes':
        return RAIL
    ww = tags.get('waterway')
    if ww == 'river':
        return RIVER
    if ww == 'stream':
        return STREAM
    nat = tags.get('natural')
    if nat == 'coastline':
        return COAST
    if nat == 'water' or ww == 'riverbank':
        return LAKE
    return None


def classify_node(tags):
    nat = tags.get('natural')
    if nat in ('peak', 'volcano'):
        return PEAK
    if tags.get('tourism') in ('alpine_hut', 'wilderness_hut'):
        return HUT
    if nat == 'spring' or tags.get('amenity') == 'drinking_water':
        return WATER
    if tags.get('railway') == 'station' and tags.get('station') != 'subway':
        return STATION
    return None


def read_osm(path, bbox):
    w, s, e, n = bbox
    lines, points = [], []
    fp = (osmium.FileProcessor(path)
          .with_locations()
          .with_filter(osmium.filter.KeyFilter('highway', 'railway', 'waterway', 'natural',
                                               'tourism', 'amenity')))
    for o in fp:
        if o.is_node():
            k = classify_node(o.tags)
            if k is None:
                continue
            lon, lat = o.location.lon, o.location.lat
            if not (w <= lon <= e and s <= lat <= n):
                continue
            points.append((k, project(lon, lat), parse_ele(o.tags.get('ele')), latin_name(o.tags)))
        elif o.is_way():
            k = classify_way(o.tags)
            if k is None:
                continue
            try:
                ll = [(nd.lon, nd.lat) for nd in o.nodes]
            except osmium.InvalidLocationError:
                continue
            if len(ll) < 2:
                continue
            if max(p[0] for p in ll) < w or min(p[0] for p in ll) > e or \
               max(p[1] for p in ll) < s or min(p[1] for p in ll) > n:
                continue
            lines.append((k, [project(lon, lat) for lon, lat in ll]))
    return lines, points


def zigzag(v):
    return (v << 1) ^ (v >> 63)


def varint(out, v):
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)


def build_level(z, kinds, tol_m, lines, points, bbox, upm):
    tsize = 1 << (WORLD_BITS - z)
    shift = max(0, (WORLD_BITS - z) - 15)  # local coordinates fit in 15 bits
    x0, y1 = project(bbox[0], bbox[1])
    x1, y0 = project(bbox[2], bbox[3])
    tx0, ty0 = int(x0) // tsize, int(y0) // tsize
    tx1, ty1 = int(x1) // tsize, int(y1) // tsize
    nx, ny = tx1 - tx0 + 1, ty1 - ty0 + 1
    tiles = [[[] for _ in range(nx)] for _ in range(ny)]
    tol = tol_m * upm

    for k, pts in lines:
        if kinds is not None and k not in kinds:
            continue
        ls = LineString(pts)
        if tol > 0:
            ls = ls.simplify(tol, preserve_topology=False)
            if ls.is_empty or ls.length < tol:
                continue
        bx0, by0, bx1, by1 = ls.bounds
        for ty in range(max(ty0, int(by0) // tsize), min(ty1, int(by1) // tsize) + 1):
            for tx in range(max(tx0, int(bx0) // tsize), min(tx1, int(bx1) // tsize) + 1):
                ox, oy = tx * tsize, ty * tsize
                part = clip_by_rect(ls, ox, oy, ox + tsize, oy + tsize)
                if part.is_empty:
                    continue
                geoms = part.geoms if hasattr(part, 'geoms') else [part]
                for g in geoms:
                    if g.geom_type != 'LineString' or len(g.coords) < 2:
                        continue
                    loc = [(min(32767, max(0, int(px - ox) >> shift)),
                            min(32767, max(0, int(py - oy) >> shift))) for px, py in g.coords]
                    # Drop repeats introduced by quantisation.
                    q = [loc[0]] + [p for i, p in enumerate(loc[1:], 1) if p != loc[i - 1]]
                    if len(q) >= 2:
                        tiles[ty - ty0][tx - tx0].append((k, q, 0, ''))

    for k, (px, py), ele, name in points:
        if kinds is not None and k not in kinds:
            continue
        if z == 9 and k == PEAK and ele < 1000:
            continue  # overview: only the big ones
        tx, ty = int(px) // tsize, int(py) // tsize
        if not (tx0 <= tx <= tx1 and ty0 <= ty <= ty1):
            continue
        loc = [((int(px) - tx * tsize) >> shift, (int(py) - ty * tsize) >> shift)]
        tiles[ty - ty0][tx - tx0].append((k, loc, ele, name))

    blobs = []
    rank = {k: i for i, k in enumerate(ORDER)}
    for row in tiles:
        for feats in row:
            feats.sort(key=lambda f: rank[f[0]])
            b = bytearray()
            varint(b, len(feats))
            for k, pts, ele, name in feats:
                b.append(k)
                if k >= PEAK:
                    varint(b, pts[0][0])
                    varint(b, pts[0][1])
                    varint(b, ele)
                    nb = name.encode('ascii')[:40]
                    b.append(len(nb))
                    b += nb
                    continue
                varint(b, len(pts))
                lx = ly = 0
                for x, y in pts:
                    varint(b, zigzag(x - lx))
                    varint(b, zigzag(y - ly))
                    lx, ly = x, y
            blobs.append(bytes(b))
    return dict(z=z, shift=shift, tx0=tx0, ty0=ty0, nx=nx, ny=ny, blobs=blobs)


def write_map(path, levels):
    head = bytearray(b'HBMAP1\0\0')
    head += struct.pack('<I', len(levels))
    desc_size = 24
    off = len(head) + desc_size * len(levels)
    index_offs = []
    for lv in levels:
        index_offs.append(off)
        off += 8 * lv['nx'] * lv['ny']
    for lv, io in zip(levels, index_offs):
        head += struct.pack('<BBHIIIII', lv['z'], lv['shift'], 0, lv['tx0'], lv['ty0'], lv['nx'],
                            lv['ny'], io)
    index = bytearray()
    data = bytearray()
    for lv in levels:
        for blob in lv['blobs']:
            index += struct.pack('<II', off + len(data), len(blob))
            data += blob
    with open(path, 'wb') as f:
        f.write(head + index + data)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    bbox = tuple(map(float, sys.argv[3:7])) if len(sys.argv) >= 7 else BBOX
    lines, points = read_osm(src, bbox)
    print(f'{len(lines)} lines, {len(points)} points', flush=True)
    by_kind = {}
    for k, _ in lines:
        by_kind[k] = by_kind.get(k, 0) + 1
    print('lines by kind', dict(sorted(by_kind.items())), flush=True)
    named = sum(1 for p in points if p[0] == PEAK and p[3])
    print(f'peaks with a latin name: {named}/{sum(1 for p in points if p[0] == PEAK)}', flush=True)
    upm = units_per_metre((bbox[1] + bbox[3]) / 2)
    levels = []
    for z, kinds, tol in LEVELS:
        lv = build_level(z, kinds, tol, lines, points, bbox, upm)
        size = sum(len(b) for b in lv['blobs'])
        print(f'level z{z}: {lv["nx"]}x{lv["ny"]} tiles, {size / 1e6:.1f} MB', flush=True)
        levels.append(lv)
    write_map(dst, levels)


if __name__ == '__main__':
    main()
