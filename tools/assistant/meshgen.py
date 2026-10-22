"""Procedural models for the Assistant's training scenes, written as .ptmesh files (the Blender
bridge format: Blender axes, Z up, the front faces -Y) with material slot names and, when asked,
vertex groups. Characters are built from simple pieces (head, neck, torso, limbs, clothes), the
way kitbashed and game-ready characters often are; props from boxes and cylinders."""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "blender", "patina_bridge"))
import protocol  # noqa: E402  (pure Python: the add-on's file format code)


class Builder:
    def __init__(self, name):
        self.name = name
        self.pos = []      # (x, y, z), Blender axes
        self.faces = []    # (vertex tuple, uv tuple, slot)
        self.slots = []    # names
        self.groups = {}   # name -> set of vertices

    def slot(self, name):
        if name not in self.slots:
            self.slots.append(name)
        return self.slots.index(name)

    def add_vert(self, p):
        self.pos.append(tuple(float(c) for c in p))
        return len(self.pos) - 1

    def add_face(self, verts, uvs, slot):
        self.faces.append((tuple(verts), tuple(uvs), slot))

    def orient(self):
        """Turns round every connected piece whose faces point inward (negative signed volume about
        the piece's centre), so all faces point out whatever order the shapes were built in."""
        parent = list(range(len(self.pos)))

        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x
        for v, _, _ in self.faces:
            for a in v[1:]:
                parent[find(a)] = find(v[0])
        pieces = {}
        for i, (v, _, _) in enumerate(self.faces):
            pieces.setdefault(find(v[0]), []).append(i)
        for faces in pieces.values():
            vs = {a for i in faces for a in self.faces[i][0]}
            c = [sum(self.pos[a][k] for a in vs) / len(vs) for k in range(3)]
            vol = 0.0
            for i in faces:
                v = self.faces[i][0]
                p0 = [self.pos[v[0]][k] - c[k] for k in range(3)]
                for j in range(1, len(v) - 1):
                    p1 = [self.pos[v[j]][k] - c[k] for k in range(3)]
                    p2 = [self.pos[v[j + 1]][k] - c[k] for k in range(3)]
                    vol += (p0[0] * (p1[1] * p2[2] - p1[2] * p2[1]) - p0[1] * (p1[0] * p2[2] - p1[2] * p2[0]) +
                            p0[2] * (p1[0] * p2[1] - p1[1] * p2[0]))
            if vol < 0:
                for i in faces:
                    v, u, sl = self.faces[i]
                    self.faces[i] = (tuple(reversed(v)), tuple(reversed(u)), sl)

    def group(self, name, verts):
        if name:
            self.groups.setdefault(name, set()).update(verts)

    def write(self, path):
        self.orient()
        positions = [c for p in self.pos for c in p]
        sizes = [len(v) for v, _, _ in self.faces]
        loops = [i for v, _, _ in self.faces for i in v]
        uvs = [c for _, u, _ in self.faces for uv in u for c in uv]
        fmat = [s for _, _, s in self.faces]
        groups = [(n, sorted(v)) for n, v in sorted(self.groups.items()) if v]
        protocol.write_ptmesh(path, self.name, positions, sizes, loops, loop_uvs=uvs, face_material=fmat,
                              vertex_groups=groups or None, extra_header={"material_names": self.slots})
        return path


# ----------------------------------------------------------------------------- shapes
def _frame(direction):
    """An orthonormal frame whose third axis is `direction`."""
    d = _norm(direction)
    up = (0, 0, 1) if abs(d[2]) < 0.95 else (1, 0, 0)
    a = _norm(_cross(up, d))
    b = _cross(d, a)
    return a, b, d


def _norm(v):
    length = math.sqrt(sum(c * c for c in v)) or 1.0
    return tuple(c / length for c in v)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _add(a, b, s=1.0):
    return tuple(x + y * s for x, y in zip(a, b))


def lathe(b, slot, base, direction, profile, seg=16, group=None, cap_start=False, cap_end=False):
    """A surface of revolution around `direction` from `base`: profile = [(t, radius)] with t the
    distance along the axis. The seam vertices are shared (welded); UVs unroll it."""
    a1, a2, d = _frame(direction)
    rows = []
    for t, r in profile:
        c = _add(base, d, t)
        row = []
        for k in range(seg):
            ph = 2 * math.pi * k / seg
            off = tuple(a1[i] * math.cos(ph) * r + a2[i] * math.sin(ph) * r for i in range(3))
            row.append(b.add_vert(_add(c, off)))
        rows.append(row)
    n = len(profile)
    for i in range(n - 1):
        for k in range(seg):
            k2 = (k + 1) % seg
            u0, u1 = k / seg, (k + 1) / seg
            v0, v1 = i / (n - 1), (i + 1) / (n - 1)
            b.add_face((rows[i][k], rows[i][k2], rows[i + 1][k2], rows[i + 1][k]),
                       ((u0, v0), (u1, v0), (u1, v1), (u0, v1)), slot)
    for cap, row, t in ((cap_start, rows[0], profile[0][0]), (cap_end, rows[-1], profile[-1][0])):
        if not cap:
            continue
        centre = b.add_vert(_add(base, d, t))
        for k in range(seg):
            k2 = (k + 1) % seg
            p = (0.5 + 0.5 * math.cos(2 * math.pi * k / seg), 0.5 + 0.5 * math.sin(2 * math.pi * k / seg))
            p2 = (0.5 + 0.5 * math.cos(2 * math.pi * k2 / seg), 0.5 + 0.5 * math.sin(2 * math.pi * k2 / seg))
            tri = (row[k], centre, row[k2]) if row is rows[0] else (row[k2], centre, row[k])
            uv = (p, (0.5, 0.5), p2) if row is rows[0] else (p2, (0.5, 0.5), p)
            b.add_face(tri, uv, slot)
    verts = [v for row in rows for v in row]
    b.group(group, verts)
    return verts


def sphere(b, slot, centre, radius, seg=16, rings=10, scale=(1, 1, 1), group=None, top=1.0):
    """A UV sphere with single pole vertices (as Blender makes it). `top` < 1 keeps only the upper
    part (a cap, open at the bottom)."""
    north = b.add_vert(_add(centre, (0, 0, radius * scale[2])))
    full = top >= 1
    last = rings - 1 if full else max(1, int(round(rings * top)))
    rows = []
    for i in range(1, last + 1):
        th = math.pi * i / rings
        rows.append([b.add_vert(_add(centre, (radius * math.sin(th) * math.cos(2 * math.pi * k / seg) * scale[0],
                                              radius * math.sin(th) * math.sin(2 * math.pi * k / seg) * scale[1],
                                              radius * math.cos(th) * scale[2]))) for k in range(seg)])
    for k in range(seg):
        k2 = (k + 1) % seg
        b.add_face((north, rows[0][k], rows[0][k2]),
                   (((k + 0.5) / seg, 1), (k / seg, 1 - 1 / rings), ((k + 1) / seg, 1 - 1 / rings)), slot)
    for i in range(len(rows) - 1):
        v0, v1 = 1 - (i + 1) / rings, 1 - (i + 2) / rings
        for k in range(seg):
            k2 = (k + 1) % seg
            b.add_face((rows[i][k], rows[i + 1][k], rows[i + 1][k2], rows[i][k2]),
                       ((k / seg, v0), (k / seg, v1), ((k + 1) / seg, v1), ((k + 1) / seg, v0)), slot)
    verts = [north] + [v for r in rows for v in r]
    if full:
        south = b.add_vert(_add(centre, (0, 0, -radius * scale[2])))
        for k in range(seg):
            k2 = (k + 1) % seg
            b.add_face((rows[-1][k], south, rows[-1][k2]),
                       ((k / seg, 1 / rings), ((k + 0.5) / seg, 0), ((k + 1) / seg, 1 / rings)), slot)
        verts.append(south)
    b.group(group, verts)
    return verts


def box(b, slot, centre, size, group=None, taper=1.0):
    """A box (each side its own UV square). `taper` scales the top face."""
    cx, cy, cz = centre
    sx, sy, sz = (s / 2 for s in size)
    corners = []
    for z in (-1, 1):
        f = taper if z > 0 else 1.0
        for y in (-1, 1):
            for x in (-1, 1):
                corners.append(b.add_vert((cx + x * sx * f, cy + y * sy * f, cz + z * sz)))
    q = ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3))
    uv = ((0, 0), (1, 0), (1, 1), (0, 1))
    for face in q:
        b.add_face(tuple(corners[i] for i in face), uv, slot)
    b.group(group, corners)
    return corners


def torus(b, slot, centre, major, minor, seg=24, sides=8, group=None, axis=(0, 0, 1)):
    a1, a2, d = _frame(axis)
    rows = []
    for i in range(seg):
        th = 2 * math.pi * i / seg
        ring_c = tuple(centre[j] + (a1[j] * math.cos(th) + a2[j] * math.sin(th)) * major for j in range(3))
        out = tuple(a1[j] * math.cos(th) + a2[j] * math.sin(th) for j in range(3))
        row = []
        for k in range(sides):
            ph = 2 * math.pi * k / sides
            p = tuple(ring_c[j] + (out[j] * math.cos(ph) + d[j] * math.sin(ph)) * minor for j in range(3))
            row.append(b.add_vert(p))
        rows.append(row)
    for i in range(seg):
        i2 = (i + 1) % seg
        for k in range(sides):
            k2 = (k + 1) % sides
            b.add_face((rows[i][k], rows[i2][k], rows[i2][k2], rows[i][k2]),
                       ((i / seg, k / sides), ((i + 1) / seg, k / sides), ((i + 1) / seg, (k + 1) / sides),
                        (i / seg, (k + 1) / sides)), slot)
    verts = [v for r in rows for v in r]
    b.group(group, verts)
    return verts


# ----------------------------------------------------------------------------- characters
OUTFITS = {
    "casual": {"skin": "Skin", "torso": "Shirt_01", "legs": "Jeans", "feet": "Boots_Leather", "hair": "Hair"},
    "knight": {"skin": "Skin", "torso": "Armor_Plate", "legs": "Chainmail", "feet": "Boots_Metal", "hair": "Hair",
               "belt": "Leather_Straps"},
    "scifi": {"skin": "Skin", "torso": "Suit_Panels", "legs": "Suit_Panels", "feet": "Boots", "helmet": "Visor_Glass"},
    "game": {"skin": "M_Body_Skin", "torso": "M_Jacket", "legs": "M_Pants", "feet": "M_Shoes", "hair": "M_Hair"},
    "blender": {"skin": "Material", "torso": "Material.001", "legs": "Material.002", "feet": "Material.003"},
    "maya": {"skin": "lambert1", "torso": "blinn2", "legs": "blinn3", "feet": "phong1"},
    "fantasy": {"skin": "Skin", "torso": "Robe", "legs": "Robe", "feet": "Sandals", "hair": "Hair", "belt": "Belt"},
    "anime": {"skin": "Face", "body": "Body", "torso": "Uniform", "legs": "Skirt", "feet": "Shoes", "hair": "Hair"},
}


def character(name, outfit="casual", pose="T", stylised=1.0, groups=True, hat=False, seg=16, height=1.8):
    """A mannequin from separate pieces. `stylised` > 1 makes a bigger head (chibi at ~2)."""
    o = OUTFITS[outfit]
    b = Builder(name)
    S = {k: b.slot(v) for k, v in o.items()}
    skin = S["skin"]
    body_skin = S.get("body", skin)
    H = height
    head_r = 0.11 * H * stylised ** 0.6
    neck_top = H * (0.86 if stylised < 1.5 else 0.72)
    head_c = (0, 0, neck_top + head_r * 0.85)
    G = (lambda n: n) if groups else (lambda n: None)
    sphere(b, skin, head_c, head_r, seg, 10, scale=(0.9, 1.0, 1.1), group=G("Head"))
    if "hair" in S:
        sphere(b, S["hair"], _add(head_c, (0, 0.01, 0.02)), head_r * 1.08, seg, 10, group=G("Head"), top=0.45)
    if "helmet" in S:
        sphere(b, S["helmet"], head_c, head_r * 1.15, seg, 10, group=G("Head"), top=0.6)
    if hat:
        hs = b.slot("Hat_Felt")
        lathe(b, hs, _add(head_c, (0, 0, head_r * 0.7)), (0, 0, 1),
              [(0, head_r * 1.6), (0.01, head_r * 1.6), (0.012, head_r * 0.95), (head_r * 0.9, head_r * 0.85)], seg,
              group=G("Head"), cap_end=True)
    neck_h = H * 0.05 * (0.6 if stylised > 1.5 else 1.0)
    lathe(b, body_skin, (0, 0, neck_top - neck_h), (0, 0, 1), [(0, 0.05 * H), (neck_h, 0.045 * H)], seg,
          group=G("Neck"))
    torso_top = neck_top - neck_h
    torso_h = H * 0.3 * (0.8 if stylised > 1.5 else 1.0)
    sh = 0.13 * H
    lathe(b, S["torso"], (0, 0, torso_top - torso_h), (0, 0, 1),
          [(0, 0.11 * H), (torso_h * 0.4, 0.1 * H), (torso_h * 0.75, 0.12 * H), (torso_h * 0.95, sh),
           (torso_h, 0.06 * H)], seg, group=G("Torso"), cap_start=True, cap_end=True)
    if "belt" in S:
        torus(b, S["belt"], (0, 0, torso_top - torso_h * 0.95), 0.115 * H, 0.012 * H, 24, 6, group=G("Torso"))
    # arms
    shoulder_z = torso_top - torso_h * 0.08
    for side, sx, gname in ((1, 1, "Arm.L"), (-1, -1, "Arm.R")):
        start = (sx * sh * 0.95, 0, shoulder_z)
        if pose == "T":
            d = (sx, 0, 0)
        else:  # A pose: 45 degrees down
            d = (sx * 0.7, 0, -0.7)
        arm_len = 0.33 * H
        lathe(b, body_skin if outfit in ("anime",) else S["torso"], start, d,
              [(0, 0.04 * H), (arm_len * 0.5, 0.035 * H), (arm_len, 0.03 * H)], seg // 2, group=G(gname),
              cap_start=True)
        hand_c = _add(start, _norm(d), arm_len + 0.04 * H)
        sphere(b, skin, hand_c, 0.04 * H, seg // 2, 6, scale=(1, 0.6, 1), group=G("Hand." + gname[-1]))
    # legs
    hip_z = torso_top - torso_h
    for sx, gname in ((1, "Leg.L"), (-1, "Leg.R")):
        lathe(b, S["legs"], (sx * 0.055 * H, 0, hip_z - 0.42 * H), (0, 0, 1),
              [(0, 0.035 * H), (0.2 * H, 0.04 * H), (0.42 * H, 0.06 * H)], seg // 2, group=G(gname), cap_end=True)
        box(b, S["feet"], (sx * 0.055 * H, -0.03 * H, hip_z - 0.43 * H - 0.025 * H), (0.07 * H, 0.15 * H, 0.05 * H),
            group=G("Foot." + gname[-1]))
    return b


def figure(name, seg=24, rows=40, split_slots=True):
    """One welded surface shaped like a figure (as a sculpt would be), Y-up profile."""
    b = Builder(name)
    skin = b.slot("Skin")
    body = b.slot("Body") if split_slots else skin

    def radius(t):
        if t < 0.70:
            return 0.30 + 0.10 * t
        if t < 0.76:
            return 0.37 + (0.45 - 0.37) * (t - 0.70) / 0.06
        if t < 0.80:
            return 0.45 - (0.45 - 0.10) * (t - 0.76) / 0.04
        if t < 0.83:
            return 0.10
        u = (t - 0.83) / 0.17
        return max(0.02, 0.22 * math.sin(math.pi * (0.15 + 0.85 * u)))

    rws = []
    for i in range(rows + 1):
        t = i / rows
        rws.append([b.add_vert((radius(t) * math.cos(2 * math.pi * k / seg), radius(t) * math.sin(2 * math.pi * k / seg),
                                2.0 * t)) for k in range(seg)])
    for i in range(rows):
        t = (i + 0.5) / rows
        for k in range(seg):
            k2 = (k + 1) % seg
            b.add_face((rws[i][k], rws[i][k2], rws[i + 1][k2], rws[i + 1][k]),
                       ((k / seg, i / rows), ((k + 1) / seg, i / rows), ((k + 1) / seg, (i + 1) / rows),
                        (k / seg, (i + 1) / rows)), skin if t > 0.8 else body)
    return b


# ----------------------------------------------------------------------------- props
def crate(name, seg=None):
    b = Builder(name)
    wood, metal = b.slot("Wood_Planks"), b.slot("Metal_Trim")
    box(b, wood, (0, 0, 0.5), (1, 1, 1))
    for z in (0.05, 0.95):
        box(b, metal, (0, 0, z), (1.04, 1.04, 0.08))
    return b


def barrel(name, seg=24):
    b = Builder(name)
    wood, metal = b.slot("Barrel_Wood"), b.slot("Iron_Hoops")
    lathe(b, wood, (0, 0, 0), (0, 0, 1), [(0, 0.35), (0.25, 0.4), (0.5, 0.42), (0.75, 0.4), (1.0, 0.35)], seg,
          cap_start=True, cap_end=True)
    for z, r in ((0.12, 0.38), (0.5, 0.425), (0.88, 0.38)):
        torus(b, metal, (0, 0, z), r, 0.015, seg, 6)
    return b


def sword(name, seg=12):
    b = Builder(name)
    blade, guard, grip = b.slot("Blade_Steel"), b.slot("Guard_Brass"), b.slot("Grip_Leather")
    box(b, blade, (0, 0, 0.6), (0.06, 0.012, 0.9), taper=0.2)
    box(b, guard, (0, 0, 0.13), (0.3, 0.04, 0.04))
    lathe(b, grip, (0, 0, -0.08), (0, 0, 1), [(0, 0.018), (0.2, 0.02)], seg)
    sphere(b, guard, (0, 0, -0.1), 0.03, seg, 6)
    return b


def shield(name, seg=24):
    b = Builder(name)
    wood, metal, paint = b.slot("Shield_Wood"), b.slot("Rim_Iron"), b.slot("Emblem_Paint")
    lathe(b, wood, (0, 0.02, 0.6), (0, -1, 0), [(0, 0.45), (0.04, 0.45)], seg, cap_start=True, cap_end=True)
    torus(b, metal, (0, 0, 0.6), 0.45, 0.02, seg, 6, axis=(0, 1, 0))
    sphere(b, paint, (0, -0.03, 0.6), 0.08, seg // 2, 6, scale=(1, 0.5, 1))
    return b


def helmet(name, seg=24):
    b = Builder(name)
    metal, padding, visor = b.slot("Helmet_Steel"), b.slot("Padding_Cloth"), b.slot("Visor")
    sphere(b, metal, (0, 0, 0), 0.15, seg, 12, top=0.55)
    sphere(b, padding, (0, 0, -0.005), 0.14, seg, 12, top=0.5)
    box(b, visor, (0, -0.14, -0.02), (0.18, 0.02, 0.05))
    return b


def table(name, seg=None):
    b = Builder(name)
    top, legs = b.slot("Table_Top"), b.slot("Table_Legs")
    box(b, top, (0, 0, 0.75), (1.6, 0.9, 0.05))
    for x in (-0.7, 0.7):
        for y in (-0.38, 0.38):
            box(b, legs, (x, y, 0.37), (0.06, 0.06, 0.74))
    return b


def robot(name, seg=12):
    b = Builder(name)
    body, joints, eyes = b.slot("Robot_Paint"), b.slot("Robot_Joints"), b.slot("Eye_Glow")
    box(b, body, (0, 0, 1.0), (0.5, 0.35, 0.6))
    box(b, body, (0, 0, 1.45), (0.3, 0.28, 0.25), group="Head")
    for x in (-0.07, 0.07):
        sphere(b, eyes, (x, -0.14, 1.48), 0.03, seg, 6)
    for x in (-0.33, 0.33):
        sphere(b, joints, (x, 0, 1.22), 0.06, seg, 6)
        box(b, body, (x, 0, 0.95), (0.1, 0.1, 0.45))
    for x in (-0.12, 0.12):
        box(b, body, (x, 0, 0.35), (0.14, 0.14, 0.6))
        sphere(b, joints, (x, 0, 0.68), 0.06, seg, 6)
    return b


PROPS = {"crate": crate, "barrel": barrel, "sword": sword, "shield": shield, "helmet": helmet, "table": table,
         "robot": robot}
