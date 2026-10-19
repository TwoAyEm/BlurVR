#!/usr/bin/env python3
"""Offline study of foveation layouts from the headset's distortion function.

    python tools/lens_dump  ... (see tools/lens_dump.cpp)  ->  lens_eye0.csv, lens_eye1.csv, hidden_eye0.csv, hidden_eye1.csv
    python tools/lens_layouts.py <folder with the csv files> [--tolerance]

The flat image each eye renders is shown through the lens warp.  The area of flat image per unit of panel area is smallest at
the sharpest spot (a flat grid that is 1:1 with the panel there is the reference, cost 1.0); everywhere else the same grid is
oversampled.  need = smallest area / local area (0..1) is the pixel density a place really needs, relative to that spot.

Layouts are sets of nested rectangles (tiles), each drawn at one density; every place gets the density of the best tile that
covers it, and the cost is the summed tile area x tile density.  Tiles overlap (no holes), as the renderer cannot cheaply cut one.

With --tolerance the need is also relaxed with eccentricity (peripheral vision resolves less than the panel can show).  That is
a scenario to be judged in the headset, not a measurement.
"""
import csv
import itertools
import math
import os
import sys


def load_eye(folder, eye):
    path = os.path.join(folder, 'lens_eye%d.csv' % eye)
    with open(path) as f:
        header = f.readline().split()
    tangents = dict(zip(header[2::2], map(float, header[3::2])))
    grid_size = int(tangents.pop('grid', 64)) if 'grid' in tangents else 64
    points = {}
    with open(path) as f:
        for row in csv.reader(f):
            if not row or row[0].startswith('#') or row[0] == 'panel_u':
                continue
            u, v, tu, tv = map(float, row)
            points[(round(u * grid_size), round(v * grid_size))] = (tu, tv)
    return tangents, grid_size, points


def load_hidden(folder, eye):
    """Triangles (flat-image u, v) of the part of the flat image the lens never shows."""
    path = os.path.join(folder, 'hidden_eye%d.csv' % eye)
    if not os.path.exists(path):
        return []
    vertices = [tuple(map(float, r)) for r in list(csv.reader(open(path)))[1:] if r]
    return [vertices[t:t + 3] for t in range(0, len(vertices) - 2, 3)]


def triangle_area(triangle):
    (ax, ay), (bx, by), (cx, cy) = triangle
    return 0.5 * abs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay))


def in_triangle(triangle, x, y):
    (ax, ay), (bx, by), (cx, cy) = triangle
    d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by)
    d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy)
    d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay)
    return not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0))


class Cell:
    __slots__ = ('u', 'v', 'lo', 'hi', 'area', 'need', 'ecc')


def build_cells(tangents, n, points, hidden):
    step = 1.0 / n
    cells = []
    for i in range(n):
        for j in range(n):
            p = [points[(j, i)], points[(j + 1, i)], points[(j, i + 1)], points[(j + 1, i + 1)]]
            if any(abs(c) > 5 for q in p for c in q):
                continue
            du = [(0.5 * (p[1][a] - p[0][a]) + 0.5 * (p[3][a] - p[2][a])) / step for a in (0, 1)]
            dv = [(0.5 * (p[2][a] - p[0][a]) + 0.5 * (p[3][a] - p[1][a])) / step for a in (0, 1)]
            cell = Cell()
            cell.area = abs(du[0] * dv[1] - du[1] * dv[0])
            cell.u = sum(q[0] for q in p) / 4
            cell.v = sum(q[1] for q in p) / 4
            if cell.area < 1e-9 or not (0 <= cell.u <= 1 and 0 <= cell.v <= 1):
                continue
            if any(in_triangle(t, cell.u, cell.v) for t in hidden):
                continue  # behind the lens barrel: needs no pixels at all
            cell.lo = (min(q[0] for q in p), min(q[1] for q in p))
            cell.hi = (max(q[0] for q in p), max(q[1] for q in p))
            cells.append(cell)
    return cells, step * step


def to_tangent(tangents, u, v):
    up = -tangents['top']  # OpenVR reports the top tangent upside down on this runtime
    down = tangents['bottom']
    if tangents['top'] > tangents['bottom']:
        up, down = tangents['top'], -tangents['bottom']
    return (tangents['left'] + u * (tangents['right'] - tangents['left']), up - v * (up + down))


def analyse(folder, eye, tolerance):
    tangents, n, points = load_eye(folder, eye)
    hidden = load_hidden(folder, eye)
    cells, weight = build_cells(tangents, n, points, hidden)
    areas = sorted(c.area for c in cells)
    smallest = areas[len(areas) // 50]
    covered = sum(c.area for c in cells) * weight
    shown = len(cells) * weight  # share of the panel that shows part of the flat image
    hidden_area = sum(triangle_area(t) for t in hidden)
    for c in cells:
        c.need = min(1.0, smallest / c.area)
    peak = [c for c in cells if c.need >= 0.97]
    pu = sum(c.u for c in peak) / len(peak)
    pv = sum(c.v for c in peak) / len(peak)
    px, py = to_tangent(tangents, pu, pv)
    print('eye %d: tangents l %.2f r %.2f up %.2f down %.2f; the lens shows %.0f%% of the flat image (the hidden-area mesh '
          'covers %.0f%%, so %.0f%% of a flat image is wasted on corners nobody sees); sharpest spot at tangents (%.2f, %.2f)' %
          (eye, tangents['left'], tangents['right'], -tangents['top'] if tangents['top'] < 0 else tangents['top'],
           tangents['bottom'], covered * 100, hidden_area * 100, hidden_area * 100, px, py))
    print('  a flat image that is 1:1 with the panel at the sharpest spot has %.2fx the panel pixels; an ideal continuous '
          'lens-matched image costs %.0f%% of it' % (1 / smallest, smallest * shown * 100))
    if tolerance:
        for c in cells:
            tx, ty = to_tangent(tangents, c.u, c.v)
            dot = (tx * px + ty * py + 1) / (math.sqrt(tx * tx + ty * ty + 1) * math.sqrt(px * px + py * py + 1))
            c.ecc = math.degrees(math.acos(max(-1.0, min(1.0, dot))))
            relax = 1.0 if c.ecc <= 15 else max(0.25, 1.0 - 0.75 * (c.ecc - 15) / 35.0)
            c.need *= relax
    return tangents, cells, (px, py), smallest * shown


def bbox_area(cells, threshold):
    lo_u = lo_v = 1.0
    hi_u = hi_v = 0.0
    count = 0
    for c in cells:
        if c.need > threshold:
            lo_u, lo_v = min(lo_u, c.lo[0]), min(lo_v, c.lo[1])
            hi_u, hi_v = max(hi_u, c.hi[0]), max(hi_v, c.hi[1])
            count += 1
    if not count:
        return 0.0, None
    lo_u, lo_v, hi_u, hi_v = max(0, lo_u), max(0, lo_v), min(1, hi_u), min(1, hi_v)
    return (hi_u - lo_u) * (hi_v - lo_v), (lo_u, hi_u, lo_v, hi_v)


def best_layouts(cells):
    candidates = [0.85, 0.7, 0.55, 0.45, 0.35, 0.28, 0.22, 0.17, 0.13, 0.1, 0.07]
    boxes = {d: bbox_area(cells, d) for d in candidates}
    results = {}
    for levels in (1, 2, 3):
        best = None
        for tail in itertools.combinations(candidates, levels):  # d_1 > ... > d_L, d_0 = 1
            densities = (1.0,) + tail
            cost = densities[-1]  # the full-image tile
            ok = True
            for k in range(levels):
                area, _ = boxes[densities[k + 1]]
                cost += area * densities[k]
            if best is None or cost < best[0]:
                best = (cost, densities)
        results[levels] = best
    return results, boxes


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    tolerance = '--tolerance' in sys.argv
    if not args:
        print(__doc__)
        return 1
    totals = {1: 0.0, 2: 0.0, 3: 0.0}
    ideal = 0.0
    for eye in (0, 1):
        tangents, cells, peak, ideal_cost = analyse(args[0], eye, tolerance)
        results, boxes = best_layouts(cells)
        ideal += ideal_cost / 2
        for levels, (cost, densities) in results.items():
            tiles = levels + 1
            linear = ', '.join('%.0f%%' % (100 * math.sqrt(d)) for d in densities)
            areas = ', '.join('%.0f%%' % (100 * boxes[d][0]) for d in densities[1:]) + ', 100%'
            print('  %d tiles per eye (%dx the draws of one): linear density %s, tile areas %s of the image -> cost %.0f%% of '
                  'a flat image' % (tiles, tiles, linear, areas, cost * 100))
            totals[levels] += cost / 2
    print('both eyes, mean cost relative to a flat image at the same centre density (%s):' %
          ('with the eccentricity tolerance' if tolerance else 'strict 1:1 need'))
    print('  ideal continuous warp %.0f%%' % (ideal * 100))
    for levels in (1, 2, 3):
        print('  %d tiles per eye: %.0f%%' % (levels + 1, totals[levels] * 100))
    return 0


if __name__ == '__main__':
    sys.exit(main())
