# Baut fertige Wegekarten aus der Kollision der Spielwelt (ffrs_extract walk) fuer Karten, deren Navmesh nur einen
# Teil abdeckt (Wunsch User 16.11.: Dilmagia, Werft, spaeter alle). Behaelt nur Flaechen, die mit Orten verbunden
# sind, an denen die Figur nachweislich steht (Ankunftspunkte, Figuren, Truhen, Speicherpunkte, Schalter), damit
# Daecher und Mauerkronen wegfallen. Aufzuege, Sperren und die Abdeckungspruefung kommen aus build_navkarten.py.
# Aufruf: build_kollkarten.py <PL-Name> [walk-Datei]
import collections
import glob
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import build_navkarten as nk  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out_full")
TALK_DIR = os.path.join(OUT, "talk")
# Figuren nicht: Statisten stehen auf Balkonen und hinter Tresen, wo die Figur nie hinkommt (Dilmagia 16.11.).
SEED_TYPES = ("MapTransitionPoint", "SavePoint", "TreasureBox", "CallSwitch", "FieldItem", "Switch")
SEED_RADIUS = 150
GATE_RADIUS = 200
# Echte Sperren; Schiebetueren gehen beim Naeherkommen von selbst auf und bleiben normale Punkte.
GATE_KINDS = ("BlockingBase", "BlockingBox", "Blocking_", "ElevatorWall", "BlockObject", "Gate", "Shutter", "Bridge", "Raft")


def actor_points(pl):
    """Lage aller Akteure der GD- und EV-Level: [(Typ, Name, (x, y, z))]."""
    base = pl[:-3]
    files = glob.glob(os.path.join(nk.GD_DIR, base + "_*GD.json")) + glob.glob(os.path.join(nk.GD_DIR, base + "_GD.json")) \
        + glob.glob(os.path.join(TALK_DIR, base + "_*EV.json")) + glob.glob(os.path.join(TALK_DIR, base + "_EV.json"))
    result = []
    for path in files:
        data = json.load(open(path, encoding="utf-8"))
        comps = {}
        for o in data:
            outer = o.get("Outer")
            if isinstance(outer, dict):
                owner = outer.get("ObjectName", "").rstrip("'").split(".")[-1]
                comps[(owner, o["Name"])] = o
        for o in data:
            props = o.get("Properties", {})
            root = props.get("RootComponent")
            if not root:
                continue
            rc = comps.get((o["Name"], root["ObjectName"].rstrip("'").split(".")[-1]))
            if not rc:
                continue
            rp = rc.get("Properties", {})
            loc = rp.get("RelativeLocation")
            if loc and not rp.get("AttachParent"):
                result.append((o["Type"], o["Name"], (loc["X"], loc["Y"], loc["Z"])))
    return result


def load_walk(path):
    grid = nk.Grid()
    rows = []
    by_key = {}
    for line in open(path, encoding="ascii"):
        p = line.split()
        ix, iy, z = int(p[0]), int(p[1]), int(p[2])
        node = nk.Node(ix, iy, z, float(p[7]), float(p[8]), None)
        grid.cells[(ix, iy)].append(node)
        by_key[(ix, iy, z)] = node
        rows.append((node, p[3:7]))
    for node, links in rows:
        for d, (dx, dy, _) in enumerate(nk.DIRS):
            if links[d] != "-":
                node.links[d] = by_key.get((node.ix + dx, node.iy + dy, int(links[d])))
    return grid


def nearest(grid, x, y, z):
    best, best_score = None, None
    r = SEED_RADIUS // nk.CELL + 1
    cx, cy = round(x / nk.CELL), round(y / nk.CELL)
    for ix in range(cx - r, cx + r + 1):
        for iy in range(cy - r, cy + r + 1):
            for n in grid.cells.get((ix, iy), ()):
                d = math.hypot(n.x - x, n.y - y)
                if d <= SEED_RADIUS and z - 250 <= n.z <= z + 60:
                    score = d + abs(z - 90 - n.z) * 0.5
                    if best is None or score < best_score:
                        best, best_score = n, score
    return best


PLATFORM_FILL = 525   # Radius um einen Aufzughalt, in dem fehlende Punkte ergaenzt werden
PLATFORM_DROP = 25    # Halt liegt etwas ueber dem Boden der Plattform (Werft: 30 ueber 5, 1638 ueber 1610)


def fill_platforms(grid, stops):
    """Fahrende Plattformen sind nicht in der Kollision: An jedem Halt fehlt die Flaeche. Mit live geprueften
    Punkten ("g") auffuellen und mit dem Boden ringsum verbinden."""
    made = []
    for name, pts in stops:
        if len(pts) < 2:
            continue
        for sx, sy, sz in pts:
            z = round(sz - PLATFORM_DROP)
            r = PLATFORM_FILL // nk.CELL + 1
            cx, cy = round(sx / nk.CELL), round(sy / nk.CELL)
            for ix in range(cx - r, cx + r + 1):
                for iy in range(cy - r, cy + r + 1):
                    x, y = ix * nk.CELL, iy * nk.CELL
                    if math.hypot(x - sx, y - sy) > PLATFORM_FILL or grid.at(ix, iy, z, nk.LINK_DZ):
                        continue
                    n = nk.Node(ix, iy, z, x, y, None, gate=True)
                    grid.cells[(ix, iy)].append(n)
                    made.append(n)
    for n in made:
        for d, (dx, dy, back) in enumerate(nk.DIRS):
            m = grid.at(n.ix + dx, n.iy + dy, n.z, nk.LINK_DZ)
            if m is not None:
                n.links[d] = m
                m.links[back] = n
    return made


def keep_reachable(grid, seeds):
    seen = set()
    queue = collections.deque(seeds)
    for s in seeds:
        seen.add(id(s))
    while queue:
        n = queue.popleft()
        for m in n.links:
            if m is not None and id(m) not in seen:
                seen.add(id(m))
                queue.append(m)
    for key in list(grid.cells):
        kept = [n for n in grid.cells[key] if id(n) in seen]
        if kept:
            grid.cells[key] = kept
        else:
            del grid.cells[key]
    for n in grid.nodes():
        n.links = [m if m is not None and id(m) in seen else None for m in n.links]
    return len(seen)


def main():
    pl = sys.argv[1]
    walk = sys.argv[2] if len(sys.argv) > 2 else os.path.join(OUT, "walk_" + pl[:-3] + ".txt")
    grid = load_walk(walk)
    total = sum(1 for _ in grid.nodes())
    actors = actor_points(pl)
    seeds, missing = [], []
    for kind, name, (x, y, z) in actors:
        if not any(t in kind for t in SEED_TYPES):
            continue
        n = nearest(grid, x, y, z)
        if n:
            seeds.append(n)
        else:
            missing.append(name)
    blocks, stops, objects = nk.load_gimmicks(pl)
    platform = fill_platforms(grid, stops)
    seeds += platform
    kept = keep_reachable(grid, seeds)
    print(f"{pl}: {total} Punkte aus der Kollision, {len(seeds)} Startorte, erreichbar {kept}; "
          f"ohne Boden: {', '.join(missing[:8])}{' ...' if len(missing) > 8 else ''}")
    gates = 0
    for kind, name, (x, y, z) in actors:
        if any(g in kind for g in GATE_KINDS) and "Door" not in kind:
            for n in grid.nodes():
                if abs(n.x - x) <= GATE_RADIUS and abs(n.y - y) <= GATE_RADIUS and abs(n.z - z) <= 400 \
                        and math.hypot(n.x - x, n.y - y) <= GATE_RADIUS and not n.gate:
                    n.gate = True
                    gates += 1
    plat = len(platform)
    made = nk.bridge(grid, blocks, stops)
    share, missing_objects = nk.coverage(grid, objects)
    target = os.path.join(nk.TARGET, "karte_" + pl + ".txt")
    nk.write(grid, target, stops)
    count = sum(1 for _ in grid.nodes())
    print(f"{pl}: {count} Punkte geschrieben, {gates} Sperrpunkte, {len(stops)} Aufzuege, {plat} Plattformpunkte, "
          f"{made} Brueckenpunkte, Abdeckung {share:.0%} von {len(objects)}"
          + (f", fehlt {', '.join(missing_objects[:6])}" if missing_objects else ""))


if __name__ == "__main__":
    main()
