// Patina Web - UV islands: the Island brush pipeline of the desktop app, in JavaScript.
// Works on the non-indexed geometry the web app uses (three corners per triangle, one UV per
// corner). Vertices at the same position are welded for topology, so a region's interior has no
// seams of its own; cutting a region out of its old island needs no seam flags (corners are
// already separate).
//
// Per-triangle flags (Uint8Array, same bits as the desktop app):
//   1  mirror copy: the triangle's UVs are taken from the other half (stacked on purpose)
//   2/4/8  the axes it was mirrored across (X / Y / Z)
//   16 pinned: UV edits leave it where it is

export const F_COPY = 1;
export const F_AXES = 14;
export const F_MIRROR = 15;
export const F_PIN = 16;

// ------------------------------------------------------------------ topology

export class Topo {
  constructor(geometry) {
    const P = geometry.attributes.position.array;
    const T = (this.T = P.length / 9);
    const canon = (this.canon = new Int32Array(3 * T));
    const ids = new Map();
    const pos = [];
    const q = 1e-6;
    for (let c = 0; c < 3 * T; c++) {
      const x = P[c * 3], y = P[c * 3 + 1], z = P[c * 3 + 2];
      const key = Math.round(x / q) + ',' + Math.round(y / q) + ',' + Math.round(z / q);
      let id = ids.get(key);
      if (id === undefined) { id = pos.length / 3; ids.set(key, id); pos.push(x, y, z); }
      canon[c] = id;
    }
    const V = (this.V = pos.length / 3);
    this.pos = Float64Array.from(pos);
    // which side of a mirror plane a point is on: within this of the plane is on it
    let r = 0;
    for (let i = 0; i < pos.length; i++) r = Math.max(r, Math.abs(pos[i]));
    this.eps = Math.max(r, 1e-6) * 1e-4;
    // vertex -> triangles
    const cnt = new Int32Array(V + 1);
    for (let c = 0; c < 3 * T; c++) cnt[canon[c] + 1]++;
    for (let v = 0; v < V; v++) cnt[v + 1] += cnt[v];
    this.vtStart = cnt;
    this.vtTris = new Int32Array(3 * T);
    const fill = cnt.slice(0, V);
    for (let c = 0; c < 3 * T; c++) this.vtTris[fill[canon[c]]++] = (c / 3) | 0;
    // triangle centers, normals, areas
    this.center = new Float64Array(3 * T);
    this.normal = new Float64Array(3 * T);
    this.area = new Float64Array(T);
    for (let t = 0; t < T; t++) {
      const a = this.p(canon[3 * t]), b = this.p(canon[3 * t + 1]), c = this.p(canon[3 * t + 2]);
      for (let k = 0; k < 3; k++) this.center[3 * t + k] = (a[k] + b[k] + c[k]) / 3;
      const u = sub(b, a), w = sub(c, a), n = cross(u, w), l = len(n);
      this.area[t] = l / 2;
      if (l > 0) for (let k = 0; k < 3; k++) this.normal[3 * t + k] = n[k] / l;
    }
    // edges: canonical pair -> triangles; shortest edge per vertex (mirror matching tolerance)
    this.edges = new Map();
    const reach = (this.reach = new Float64Array(V).fill(Infinity));
    for (let t = 0; t < T; t++)
      for (let k = 0; k < 3; k++) {
        const a = canon[3 * t + k], b = canon[3 * t + ((k + 1) % 3)];
        if (a === b) continue;
        const key = a < b ? a * V + b : b * V + a;
        let list = this.edges.get(key);
        if (!list) this.edges.set(key, (list = []));
        list.push(t);
        const e = len(sub(this.p(a), this.p(b)));
        if (e > 0) { reach[a] = Math.min(reach[a], e); reach[b] = Math.min(reach[b], e); }
      }
    const floorTol = 2e-5;
    for (let v = 0; v < V; v++) reach[v] = isFinite(reach[v]) ? Math.max(reach[v] * 0.3, floorTol) : floorTol;
    this._mirror = {};
  }

  p(v) { return [this.pos[3 * v], this.pos[3 * v + 1], this.pos[3 * v + 2]]; }
  c(t) { return [this.center[3 * t], this.center[3 * t + 1], this.center[3 * t + 2]]; }
  // the corner (0..2) of triangle t at canonical vertex v, or -1
  cornerAt(t, v) {
    for (let k = 0; k < 3; k++) if (this.canon[3 * t + k] === v) return k;
    return -1;
  }
  trisAt(v) { return this.vtTris.subarray(this.vtStart[v], this.vtStart[v + 1]); }
}

const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const len = (a) => Math.hypot(a[0], a[1], a[2]);
const flip = (p, mask) => [mask & 1 ? -p[0] : p[0], mask & 2 ? -p[1] : p[1], mask & 4 ? -p[2] : p[2]];
const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);

// ------------------------------------------------------------------ UV islands

// triangles connected through edges whose UVs agree on both sides
export function uvIslands(topo, uv) {
  const T = topo.T, parent = new Int32Array(T);
  for (let t = 0; t < T; t++) parent[t] = t;
  const find = (x) => { while (parent[x] !== x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
  const same = (c1, c2) => Math.abs(uv[2 * c1] - uv[2 * c2]) < 1e-6 && Math.abs(uv[2 * c1 + 1] - uv[2 * c2 + 1]) < 1e-6;
  const V = topo.V;
  for (const [key, tris] of topo.edges) {
    if (tris.length < 2) continue;
    const a = Math.floor(key / V), b = key - a * V;
    for (let i = 0; i < tris.length; i++)
      for (let j = i + 1; j < tris.length; j++) {
        const t1 = tris[i], t2 = tris[j];
        const a1 = 3 * t1 + topo.cornerAt(t1, a), b1 = 3 * t1 + topo.cornerAt(t1, b);
        const a2 = 3 * t2 + topo.cornerAt(t2, a), b2 = 3 * t2 + topo.cornerAt(t2, b);
        if (same(a1, a2) && same(b1, b2)) parent[find(t1)] = find(t2);
      }
  }
  const id = new Int32Array(T), remap = new Map();
  for (let t = 0; t < T; t++) {
    const r = find(t);
    let i = remap.get(r);
    if (i === undefined) { i = remap.size; remap.set(r, i); }
    id[t] = i;
  }
  return { id, count: remap.size };
}

export function triUVArea(uv, t) {
  const o = 6 * t;
  return ((uv[o + 2] - uv[o]) * (uv[o + 5] - uv[o + 1]) - (uv[o + 4] - uv[o]) * (uv[o + 3] - uv[o + 1])) / 2;
}

// sqrt(UV area / 3D area) over the triangles: UV units per model unit
export function density(topo, uv, tris) {
  let a3 = 0, auv = 0;
  for (const t of tris) { a3 += topo.area[t]; auv += Math.abs(triUVArea(uv, t)); }
  return a3 > 0 && auv > 0 ? Math.sqrt(auv / a3) : 0;
}

export function islandLists(isl) {
  const lists = Array.from({ length: isl.count }, () => []);
  for (let t = 0; t < isl.id.length; t++) lists[isl.id[t]].push(t);
  return lists;
}

// ------------------------------------------------------------------ mirror matching
// A triangle's mirror image is found from its corners: each vertex is mirrored and matched to
// the nearest vertex within a fraction of its shortest edge. That works where the two halves are
// triangulated differently (quads split along other diagonals), which matching whole faces can't.

// canonical vertex -> its mirror image (-1: none): nearest within a third of the shortest edge,
// then (where the halves differ in shape but are built the same way) grown along the mesh: an
// unmatched vertex takes the vertex whose neighbors mirror its own matched neighbors
function mirrorVertexMap(topo, mask) {
  if (topo._mirror[mask]) return topo._mirror[mask];
  const V = topo.V;
  if (!topo._adj) {
    // all neighbors, and the neighbors along quad edges: the diagonal of a quad split into two
    // triangles (the longest edge of both, nearly flat) is left out, because the two halves of a
    // model are often split along different diagonals
    const nb = Array.from({ length: V }, () => new Set()), nq = Array.from({ length: V }, () => new Set());
    const elen = (a, b) => dist(topo.p(a), topo.p(b));
    const longest = (t, a, b) => {
      const L = elen(a, b);
      for (let k = 0; k < 3; k++) {
        const x = topo.canon[3 * t + k], y = topo.canon[3 * t + ((k + 1) % 3)];
        if ((x === a && y === b) || (x === b && y === a)) continue;
        if (elen(x, y) > L * 1.0001) return false;
      }
      return true;
    };
    for (const [key, tris] of topo.edges) {
      const a = Math.floor(key / V), b = key - a * V;
      nb[a].add(b); nb[b].add(a);
      let diag = false;
      if (tris.length === 2) {
        const [t1, t2] = tris;
        const n1 = [topo.normal[3 * t1], topo.normal[3 * t1 + 1], topo.normal[3 * t1 + 2]], n2 = [topo.normal[3 * t2], topo.normal[3 * t2 + 1], topo.normal[3 * t2 + 2]];
        diag = dot(n1, n2) > 0.85 && longest(t1, a, b) && longest(t2, a, b);
      }
      if (!diag) { nq[a].add(b); nq[b].add(a); }
    }
    const sorted = (s) => Int32Array.from([...s].sort((x, y) => x - y));
    topo._adj = nb.map(sorted);
    topo._adjQ = nq.map(sorted);
  }
  const adj = topo._adj, adjQ = topo._adjQ;
  // uniform grid over the vertices
  let cell = 0;
  { const r = Array.from(topo.reach).sort((a, b) => a - b); cell = Math.max(r[r.length >> 1] || 1e-3, 1e-5); }
  const grid = new Map();
  const key = (x, y, z) => x + ',' + y + ',' + z;
  for (let v = 0; v < V; v++) {
    const k = key(Math.floor(topo.pos[3 * v] / cell), Math.floor(topo.pos[3 * v + 1] / cell), Math.floor(topo.pos[3 * v + 2] / cell));
    let l = grid.get(k);
    if (!l) grid.set(k, (l = []));
    l.push(v);
  }
  // sides of the (first) mirror plane: negative vertices look for partners on the positive side
  const axis = mask & 1 ? 0 : mask & 2 ? 1 : 2, eps = topo.eps;
  const side = (v) => { const x = topo.pos[3 * v + axis]; return x < -eps ? -1 : x > eps ? 1 : 0; };
  const map = new Int32Array(V).fill(-1), md = new Float64Array(V).fill(Infinity);
  const owner = new Int32Array(V).fill(-1);
  // the image of a vertex on either side (the map is built from the negative side)
  const inv = new Int32Array(V).fill(-1);
  const image = (n) => (side(n) > 0 ? inv[n] : map[n]);
  // unmatched vertices take the nearest free vertex within `frac` of their reach
  const seed = (frac) => {
    for (let v = 0; v < V; v++) {
      const sv = side(v);
      if (sv > 0 || map[v] >= 0) continue;
      const p = flip(topo.p(v), mask), tol = topo.reach[v] * frac;
      const r = Math.ceil(tol / cell);
      const cx = Math.floor(p[0] / cell), cy = Math.floor(p[1] / cell), cz = Math.floor(p[2] / cell);
      let best = -1, bd = tol;
      for (let dz = -r; dz <= r; dz++)
        for (let dy = -r; dy <= r; dy++)
          for (let dx = -r; dx <= r; dx++) {
            const l = grid.get(key(cx + dx, cy + dy, cz + dz));
            if (!l) continue;
            for (const w of l) {
              if (sv < 0 ? side(w) < 0 : side(w) !== 0) continue;
              if (owner[w] >= 0) continue;
              const d = dist(topo.p(w), p);
              if (d <= bd) { bd = d; best = w; }
            }
          }
      if (best >= 0) { map[v] = best; md[v] = bd; }
    }
  };
  const adjacent = (a, b) => {
    if (a === b) return true;
    const l = adjQ[a];
    let lo = 0, hi = l.length - 1;
    while (lo <= hi) { const mid = (lo + hi) >> 1; if (l[mid] === b) return true; if (l[mid] < b) lo = mid + 1; else hi = mid - 1; }
    return false;
  };
  const agreement = (v, w) => {
    let agree = 0, matched = 0;
    for (const n of adjQ[v]) { const mn = image(n); if (mn < 0) continue; matched++; if (adjacent(mn, w)) agree++; }
    return [agree, matched];
  };
  const rebuildOwners = () => {
    owner.fill(-1); inv.fill(-1);
    for (let v = 0; v < V; v++) {
      if (map[v] < 0) continue;
      const o = owner[map[v]];
      if (o < 0) owner[map[v]] = v;
      else if (md[v] < md[o]) { map[o] = -1; owner[map[v]] = v; } else map[v] = -1;
    }
    for (let w = 0; w < V; w++) if (owner[w] >= 0 && side(w) > 0) inv[w] = owner[w];
  };
  // matches most of whose matched neighbors disagree go, until it settles
  const cleanup = () => {
    for (let pass = 0; pass < 10; pass++) {
      const drop = [];
      for (let v = 0; v < V; v++) {
        if (map[v] < 0 || side(v) === 0) continue;
        const [agree, matched] = agreement(v, map[v]);
        if (matched >= 2 && agree * 2 < matched) drop.push(v);
      }
      if (!drop.length) break;
      for (const v of drop) map[v] = -1;
      rebuildOwners();
    }
  };
  // (0.43) the spacing of the vertices round each one: its second shortest edge, so the rim of a cloth with
  // thickness (an edge a millimeter long to the layer under it) doesn't count; and its average edge
  const spacing = new Float64Array(V), meanEdge = new Float64Array(V);
  for (let v = 0; v < V; v++) {
    let a = Infinity, b = Infinity, sum = 0;
    for (const n of adjQ[v]) {
      const d = dist(topo.p(n), topo.p(v));
      sum += d;
      if (d < a) { b = a; a = d; } else if (d < b) b = d;
    }
    const shortest = topo.reach[v] / 0.3;
    spacing[v] = b < Infinity ? b : a < Infinity ? a : shortest;
    meanEdge[v] = adjQ[v].length ? sum / adjQ[v].length : shortest;
  }
  // (0.43) on a model that isn't exactly symmetric the vertices down the middle wander off the plane
  // (millimeters on a cloth-simmed dress, whole panels shifted to one side): such a vertex is its own mirror
  // image, so it may match itself, on either side, less than half its average edge from the plane, when the
  // images of its neighbors are around it (the prediction from them decides, as for any other match)
  const nearMiddle = (v) => Math.abs(topo.pos[3 * v + axis]) < 0.5 * meanEdge[v];
  const unmatched = (v) => {
    if (map[v] >= 0) return false;
    const sv = side(v);
    return sv < 0 || (sv > 0 && owner[v] < 0 && nearMiddle(v));
  };
  // grow along the mesh. An unmatched vertex's mirror image is predicted from its matched
  // neighbors: its own mirrored position moved by how far each neighbor's image sits from that
  // neighbor's mirrored position (the local difference in shape between the halves). The
  // candidates are the unclaimed neighbors of those images; the one nearest the prediction wins
  // when it agrees with the most matched neighbors and is clearly nearer than the runner-up.
  const grow = () => {
    // only vertices next to a new match are looked at again
    let work = [];
    for (let v = 0; v < V; v++) if (unmatched(v)) work.push(v);
    for (let pass = 0; pass < 100000 && work.length; pass++) {
      const adds = [];
      for (const v of work) {
        if (!unmatched(v)) continue;
        const pv = flip(topo.p(v), mask), pred = [0, 0, 0], imgs = [];
        for (const n of adjQ[v]) {
          const mn = image(n);
          if (mn < 0) continue;
          const fn = flip(topo.p(n), mask), pm = topo.p(mn);
          for (let k = 0; k < 3; k++) pred[k] += pv[k] + pm[k] - fn[k];
          imgs.push(mn);
        }
        if (!imgs.length) continue;
        for (let k = 0; k < 3; k++) pred[k] /= imgs.length;
        const cands = new Map();
        for (const mn of imgs)
          for (const w of adjQ[mn]) {
            const self = w === v && nearMiddle(v);  // (a vertex off the plane on the positive side: only itself)
            if (owner[w] >= 0 || ((side(w) < 0 || side(v) > 0) && !self)) continue;
            cands.set(w, (cands.get(w) || 0) + 1);
          }
        let bestW = -1, bestA = 0, bestD = Infinity, secondD = Infinity;
        for (const [w, a] of cands) {
          const d = dist(topo.p(w), pred);
          if (a > bestA || (a === bestA && d < bestD)) {
            if (a === bestA) secondD = bestD; else secondD = Infinity;
            bestA = a; bestD = d; bestW = w;
          } else if (a === bestA && d < secondD) secondD = d;
        }
        if (bestW < 0) continue;
        const e = Math.min(spacing[v], spacing[bestW]);
        // one matched neighbor says little: the prediction must then be close and unambiguous
        const tight = bestA >= 2 ? 0.6 : 0.35;
        if (bestD > tight * e * 2 || secondD < bestD * 1.8) continue;
        adds.push([v, bestW, bestA * 1e6 - bestD]);
      }
      if (!adds.length) break;
      adds.sort((a, b) => b[2] - a[2]);
      // the most certain half of each pass goes in first, so the rest see better predictions
      const take = Math.max(1, Math.ceil(adds.length / 2));
      let n = 0;
      const next = new Set();
      for (const [v, w] of adds) {
        if (n >= take) { next.add(v); continue; }
        if (owner[w] >= 0 || map[v] >= 0) continue;
        owner[w] = v; map[v] = w; md[v] = 0; if (side(w) > 0) inv[w] = v; n++;
        for (const x of adjQ[v]) if (unmatched(x)) next.add(x);
        for (const y of adjQ[w]) for (const x of adjQ[y]) if (unmatched(x)) next.add(x);
        for (const x of adjQ[w]) if (unmatched(x)) next.add(x);
      }
      work = [...next];
    }
  };
  // First the vertices with an exact partner, grown out across the mesh from there; that keeps a
  // half that is out of shape by a whole edge from matching the neighboring row. Then, for parts
  // with no exact partner anywhere, near partners (a third of the shortest edge), grown again.
  let seeded = 0;
  for (const frac of [0.05, 1]) {
    seed(frac);
    rebuildOwners();
    cleanup();
    if (frac < 1) for (let v = 0; v < V; v++) if (map[v] >= 0) seeded++;
    grow();
  }
  topo._mirrorStats = topo._mirrorStats || {};
  { let all = 0; for (let v = 0; v < V; v++) if (map[v] >= 0) all++; topo._mirrorStats[mask] = { exact: seeded, matched: all, negative: V }; }
  // both ways
  for (let v = 0; v < V; v++) if (side(v) < 0 && map[v] >= 0 && side(map[v]) > 0) map[map[v]] = v;
  topo._mirror[mask] = map;
  return map;
}

export function negMask(topo, t, axes) {
  const eps = topo.eps;
  let m = 0;
  for (let a = 0; a < 3; a++) if (axes & (1 << a) && topo.center[3 * t + a] < -eps) m |= 1 << a;
  return m;
}

// the triangle(s) a triangle maps onto across `mask`
export function mirrorTris(topo, t, mask) {
  mask &= 7;
  if (!mask) return [t];
  const map = mirrorVertexMap(topo, mask);
  const fv = [topo.canon[3 * t], topo.canon[3 * t + 1], topo.canon[3 * t + 2]];
  const W = [];
  let all = true;
  for (const v of fv) { const m = map[v]; if (m < 0) all = false; else if (!W.includes(m)) W.push(m); }
  if (all && W.length === 3) {
    for (const g of topo.trisAt(W[0])) {
      const gv = [topo.canon[3 * g], topo.canon[3 * g + 1], topo.canon[3 * g + 2]];
      if (W.every((w) => gv.includes(w))) return [g];
    }
  }
  if (W.length >= 2) {
    // triangulated differently on the other side: t is half of a quad split along one diagonal and
    // the image of the quad is split along the other. The image triangles are those sharing two of
    // t's mirrored corners whose corners all mirror a corner of the quad
    const adjQ = topo._adjQ, quad = new Set(fv);
    const isQuadEdge = (a, b) => adjQ[a].includes(b);
    for (let k = 0; k < 3; k++) {
      const a = fv[k], b = fv[(k + 1) % 3];
      if (isQuadEdge(a, b)) continue;  // a diagonal: the triangle across it is the quad's other half
      for (const g of topo.edges.get(a < b ? a * topo.V + b : b * topo.V + a) || [])
        for (let j = 0; j < 3; j++) quad.add(topo.canon[3 * g + j]);
    }
    const M = new Set();
    for (const v of quad) if (map[v] >= 0) M.add(map[v]);
    const out = [], seen = new Set();
    for (const w of W)
      for (const g of topo.trisAt(w)) {
        if (seen.has(g)) continue;
        seen.add(g);
        let shared = 0, inside = true;
        for (let k = 0; k < 3; k++) { const x = topo.canon[3 * g + k]; if (W.includes(x)) shared++; if (!M.has(x)) inside = false; }
        if (shared >= 2 && inside) out.push(g);
      }
    if (out.length) return out;
    // no quad found (a strongly bent quad, or a triangle fan): the triangles sharing two mirrored
    // corners whose mirrored center is nearer t than any of t's edge neighbors
    const near = [t];
    for (const v of fv)
      for (const g of topo.trisAt(v)) {
        if (near.includes(g)) continue;
        let sh = 0;
        for (let k = 0; k < 3; k++) if (fv.includes(topo.canon[3 * g + k])) sh++;
        if (sh >= 2) near.push(g);
      }
    for (const g of seen) {
      let shared = 0;
      for (let k = 0; k < 3; k++) if (W.includes(topo.canon[3 * g + k])) shared++;
      if (shared < 2) continue;
      const mc = flip(topo.c(g), mask);
      const df = dist(topo.c(t), mc);
      let best = Infinity;
      for (const n of near) best = Math.min(best, dist(topo.c(n), mc));
      if (df <= best * 1.001 + 1e-12) out.push(g);
    }
    if (out.length) return out;
  }
  return [];
}

// selection (Uint8Array per triangle) plus its mirror images for every combination of the axes;
// where the halves are triangulated differently, the other half of a split quad joins so that
// every triangle on the negative side has all of its mirrored corners on selected triangles
export function mirrorSelection(topo, sel, axes) {
  if (!(axes & 7)) return sel;
  const add = (t) => {
    const q = [t];
    sel[t] = 1;
    for (let i = 0; i < q.length; i++)
      for (let sub = 1; sub < 8; sub++) {
        if ((sub & axes) !== sub) continue;
        for (const g of mirrorTris(topo, q[i], sub)) if (!sel[g]) { sel[g] = 1; q.push(g); }
      }
  };
  for (let t = 0; t < topo.T; t++) if (sel[t]) add(t);
  for (let pass = 0; pass < 4; pass++) {
    const extra = [];
    for (let t = 0; t < topo.T; t++) {
      if (!sel[t]) continue;
      const neg = negMask(topo, t, axes);
      if (!neg) continue;
      const map = mirrorVertexMap(topo, neg);
      const mc = flip(topo.c(t), neg);
      const mvs = [0, 1, 2].map((k) => map[topo.canon[3 * t + k]]);
      for (let k = 0; k < 3; k++) {
        const mv = mvs[k];
        if (mv < 0) continue;
        // the triangle holding the most of t's mirrored corners (the other half of the split
        // quad), then one sharing an edge with the selection, then the nearest
        let covered = false, best = -1, bs = -Infinity;
        for (const g of topo.trisAt(mv)) {
          if (negMask(topo, g, axes)) continue;
          if (sel[g]) { covered = true; break; }
          const gv = [topo.canon[3 * g], topo.canon[3 * g + 1], topo.canon[3 * g + 2]];
          let held = 0, touching = 0;
          for (const w of gv) if (mvs.includes(w)) held++;
          for (let e = 0; e < 3 && !touching; e++) {
            const a = gv[e], b = gv[(e + 1) % 3];
            for (const h of topo.edges.get(a < b ? a * topo.V + b : b * topo.V + a) || []) if (h !== g && sel[h]) touching = 1;
          }
          const score = held * 10 + touching * 5 - dist(topo.c(g), mc);
          if (score > bs) { bs = score; best = g; }
        }
        if (!covered && best >= 0) extra.push(best);
      }
    }
    if (!extra.length) break;
    for (const g of extra) add(g);
  }
  return sel;
}

// for each corner of triangle t: the corner (global index 3*tri+k) it mirrors onto across
// `mask`, on a triangle `accept` allows; null if one is missing. Several triangles meet at a
// vertex: the one whose corner already has the copy's UV wins (checked in each of `uvs` in turn,
// so a copy stays linked to the half it was stacked on), then the one nearest the mirrored center.
export function mirrorCorners(topo, t, mask, accept, uvs = []) {
  const map = mirrorVertexMap(topo, mask);
  const mc = flip(topo.c(t), mask);
  const out = [0, 0, 0];
  for (let k = 0; k < 3; k++) {
    const mv = map[topo.canon[3 * t + k]];
    if (mv < 0) return null;
    const c = 3 * t + k;
    let best = -1, bs = Infinity;
    for (const g of topo.trisAt(mv)) {
      if (g === t || !accept(g)) continue;
      const s = 3 * g + topo.cornerAt(g, mv);
      let rank = uvs.length;
      for (let i = 0; i < uvs.length; i++) {
        const u = uvs[i];
        if (Math.abs(u[2 * c] - u[2 * s]) < 1e-6 && Math.abs(u[2 * c + 1] - u[2 * s + 1]) < 1e-6) { rank = i; break; }
      }
      const score = rank * 1e9 + dist(topo.c(g), mc);
      if (score < bs) { bs = score; best = s; }
    }
    if (best < 0) return null;
    out[k] = best;
  }
  return out;
}

// copy triangle -> the corners it takes its UVs from (Int32Array 3T, -1 = not a copy). `uvs`:
// layouts whose stacking decides between candidate sources (see mirrorCorners)
export function mirrorLinks(topo, flags, uvs = []) {
  const src = new Int32Array(3 * topo.T).fill(-1);
  let copies = 0;
  const notCopy = (g) => !(flags[g] & F_COPY);
  for (let t = 0; t < topo.T; t++) {
    if (!(flags[t] & F_COPY)) continue;
    const mask = (flags[t] >> 1) & 7;
    const s = mask ? mirrorCorners(topo, t, mask, notCopy, uvs) : null;
    if (!s) { flags[t] &= ~F_MIRROR; continue; }  // stale
    src.set(s, 3 * t);
    copies++;
  }
  return { src, copies };
}

// copies take their source's UVs; returns the number of triangles changed
export function restack(uv, links) {
  let n = 0;
  for (let t = 0; t * 3 < links.src.length; t++) {
    if (links.src[3 * t] < 0) continue;
    let changed = false;
    for (let k = 0; k < 3; k++) {
      const s = links.src[3 * t + k], c = 3 * t + k;
      if (uv[2 * c] !== uv[2 * s] || uv[2 * c + 1] !== uv[2 * s + 1]) { uv[2 * c] = uv[2 * s]; uv[2 * c + 1] = uv[2 * s + 1]; changed = true; }
    }
    if (changed) n++;
  }
  return n;
}

export function unstackedCount(uv, links) {
  let n = 0;
  for (let t = 0; t * 3 < links.src.length; t++) {
    if (links.src[3 * t] < 0) continue;
    for (let k = 0; k < 3; k++) {
      const s = links.src[3 * t + k], c = 3 * t + k;
      if (Math.hypot(uv[2 * c] - uv[2 * s], uv[2 * c + 1] - uv[2 * s + 1]) > 1e-6) { n++; break; }
    }
  }
  return n;
}

// triangles plus their linked halves (copies of them, and what the copies copy)
export function withLinked(topo, links, tris) {
  const inS = new Uint8Array(topo.T);
  for (const t of tris) inS[t] = 1;
  for (const t of tris) if (links.src[3 * t] >= 0) for (let k = 0; k < 3; k++) inS[(links.src[3 * t + k] / 3) | 0] = 1;
  for (let t = 0; t < topo.T; t++)
    if (links.src[3 * t] >= 0) for (let k = 0; k < 3; k++) if (inS[(links.src[3 * t + k] / 3) | 0]) inS[t] = 1;
  const out = [];
  for (let t = 0; t < topo.T; t++) if (inS[t]) out.push(t);
  return out;
}

// ------------------------------------------------------------------ LSCM

// Least-squares conformal map of one chart (triangles connected through welded vertices).
// Returns per-corner UVs: Map corner(3t+k) -> [u, v] as a Float64Array indexed by chart order.
function lscm(topo, tris) {
  const local = new Map();
  const P = [];
  for (const t of tris)
    for (let k = 0; k < 3; k++) {
      const v = topo.canon[3 * t + k];
      if (!local.has(v)) { local.set(v, P.length); P.push(topo.p(v)); }
    }
  const n = P.length;
  const T = tris.map((t) => [local.get(topo.canon[3 * t]), local.get(topo.canon[3 * t + 1]), local.get(topo.canon[3 * t + 2])]);
  // boundary vertices: on edges used once inside the chart
  const edgeUse = new Map();
  for (const tr of T)
    for (let k = 0; k < 3; k++) {
      const a = tr[k], b = tr[(k + 1) % 3], key = a < b ? a * n + b : b * n + a;
      edgeUse.set(key, (edgeUse.get(key) || 0) + 1);
    }
  const bnd = new Set();
  for (const [key, c] of edgeUse) if (c === 1) { const a = Math.floor(key / n); bnd.add(a); bnd.add(key - a * n); }
  let pins = bnd.size ? [...bnd] : [...Array(n).keys()];
  // farthest pair on the boundary
  let pa = pins[0], pb = pins[0];
  if (pins.length <= 3000) {
    let bd = -1;
    for (let i = 0; i < pins.length; i++)
      for (let j = i + 1; j < pins.length; j++) {
        const d = dist(P[pins[i]], P[pins[j]]);
        if (d > bd) { bd = d; pa = pins[i]; pb = pins[j]; }
      }
  } else {
    const far = (a) => { let b = a, bd = -1; for (const i of pins) { const d = dist(P[i], P[a]); if (d > bd) { bd = d; b = i; } } return b; };
    pa = far(pins[0]); pb = far(pa);
  }
  // warm start: projection on the plane through the pin axis nearest the average normal
  let N = [0, 0, 0];
  for (const tr of T) { const c = cross(sub(P[tr[1]], P[tr[0]]), sub(P[tr[2]], P[tr[0]])); N = [N[0] + c[0], N[1] + c[1], N[2] + c[2]]; }
  let e1 = sub(P[pb], P[pa]);
  const pinDist = len(e1);
  e1 = pinDist > 1e-20 ? e1.map((x) => x / pinDist) : [1, 0, 0];
  let nn = sub(N, e1.map((x) => x * dot(N, e1)));
  if (len(nn) < 1e-12 * (len(N) + 1e-30)) nn = cross(e1, Math.abs(e1[0]) < 0.9 ? [1, 0, 0] : [0, 1, 0]);
  nn = nn.map((x) => x / len(nn));
  const e2 = cross(nn, e1);
  const U = new Float64Array(2 * n);
  for (let i = 0; i < n; i++) { const d = sub(P[i], P[pa]); U[2 * i] = dot(d, e1); U[2 * i + 1] = dot(d, e2); }
  if (pinDist <= 1e-20) { U[2 * pb] = 1; U[2 * pb + 1] = 0; }
  if (n >= 3 && pa !== pb) {
    // conformal coefficients per triangle
    const W = [], valid = [];
    for (let i = 0; i < T.length; i++) {
      const tr = T[i];
      const d1 = sub(P[tr[1]], P[tr[0]]), d2 = sub(P[tr[2]], P[tr[0]]);
      const l1 = len(d1), cr = cross(d1, d2), area2 = len(cr);
      const maxE = Math.max(l1, len(d2), len(sub(d2, d1)));
      if (!(area2 > 1e-12 * maxE * maxE) || !(l1 > 0)) continue;
      const x = d1.map((a) => a / l1), y = cross(cr.map((a) => a / area2), x);
      const q1 = [l1, 0], q2 = [dot(d2, x), dot(d2, y)];
      const s = 1 / Math.sqrt(area2);
      W.push([(q2[0] - q1[0]) * s, (q2[1] - q1[1]) * s, (0 - q2[0]) * s, (0 - q2[1]) * s, (q1[0] - 0) * s, (q1[1] - 0) * s]);
      valid.push(tr);
    }
    const freeIdx = new Int32Array(n).fill(-1);
    let nFree = 0;
    for (let i = 0; i < n; i++) if (i !== pa && i !== pb) freeIdx[i] = nFree++;
    // sparse block matrix (complex-like 2x2 blocks [a -c; c a]) in row maps
    const rows = Array.from({ length: nFree }, () => new Map());
    const rhs = new Float64Array(2 * nFree), x = new Float64Array(2 * nFree);
    for (let w = 0; w < valid.length; w++) {
      const tr = valid[w], c = W[w];
      for (let j = 0; j < 3; j++) {
        const fj = freeIdx[tr[j]];
        if (fj < 0) continue;
        const wrj = c[2 * j], wij = c[2 * j + 1];
        for (let k = 0; k < 3; k++) {
          const wrk = c[2 * k], wik = c[2 * k + 1];
          const ba = wrj * wrk + wij * wik, bc = wrj * wik - wij * wrk;
          const fk = freeIdx[tr[k]];
          if (fk >= 0) {
            const e = rows[fj].get(fk);
            if (e) { e[0] += ba; e[1] += bc; } else rows[fj].set(fk, [ba, bc]);
          } else {
            const pu = U[2 * tr[k]], pv = U[2 * tr[k] + 1];
            rhs[2 * fj] -= ba * pu - bc * pv;
            rhs[2 * fj + 1] -= bc * pu + ba * pv;
          }
        }
      }
    }
    for (let i = 0; i < n; i++) if (freeIdx[i] >= 0) { x[2 * freeIdx[i]] = U[2 * i]; x[2 * freeIdx[i] + 1] = U[2 * i + 1]; }
    // CSR
    const rowPtr = new Int32Array(nFree + 1);
    for (let i = 0; i < nFree; i++) rowPtr[i + 1] = rowPtr[i] + rows[i].size;
    const col = new Int32Array(rowPtr[nFree]), A = new Float64Array(rowPtr[nFree]), C = new Float64Array(rowPtr[nFree]);
    const diag = new Float64Array(nFree);
    for (let i = 0; i < nFree; i++) {
      let k = rowPtr[i];
      for (const [j, e] of rows[i]) { col[k] = j; A[k] = e[0]; C[k] = e[1]; if (j === i) diag[i] = e[0]; k++; }
    }
    const mul = (xv, yv) => {
      for (let i = 0; i < nFree; i++) {
        let yu = 0, yw = 0;
        for (let k = rowPtr[i]; k < rowPtr[i + 1]; k++) {
          const xu = xv[2 * col[k]], xw = xv[2 * col[k] + 1];
          yu += A[k] * xu - C[k] * xw;
          yw += C[k] * xu + A[k] * xw;
        }
        yv[2 * i] = yu; yv[2 * i + 1] = yw;
      }
    };
    // Jacobi-preconditioned conjugate gradient
    const N2 = 2 * nFree, r = new Float64Array(N2), z = new Float64Array(N2), p = new Float64Array(N2), Ap = new Float64Array(N2);
    const invD = new Float64Array(N2);
    for (let i = 0; i < nFree; i++) invD[2 * i] = invD[2 * i + 1] = diag[i] > 1e-300 ? 1 / diag[i] : 0;
    mul(x, Ap);
    let bnorm = 0;
    for (let i = 0; i < N2; i++) { r[i] = rhs[i] - Ap[i]; z[i] = r[i] * invD[i]; p[i] = z[i]; bnorm += rhs[i] * rhs[i]; }
    bnorm = Math.sqrt(bnorm) || 1;
    let rz = 0;
    for (let i = 0; i < N2; i++) rz += r[i] * z[i];
    const maxIt = Math.max(200, Math.min(20000, 8 * Math.sqrt(nFree) * 10));
    for (let it = 0; it < maxIt; it++) {
      mul(p, Ap);
      let pAp = 0;
      for (let i = 0; i < N2; i++) pAp += p[i] * Ap[i];
      if (!(Math.abs(pAp) > 1e-300)) break;
      const alpha = rz / pAp;
      let rn = 0;
      for (let i = 0; i < N2; i++) { x[i] += alpha * p[i]; r[i] -= alpha * Ap[i]; rn += r[i] * r[i]; }
      if (Math.sqrt(rn) < 1e-10 * bnorm) break;
      let rz2 = 0;
      for (let i = 0; i < N2; i++) { z[i] = r[i] * invD[i]; rz2 += r[i] * z[i]; }
      const beta = rz2 / rz;
      rz = rz2;
      for (let i = 0; i < N2; i++) p[i] = z[i] + beta * p[i];
    }
    if (x.every((v) => isFinite(v)))
      for (let i = 0; i < n; i++) if (freeIdx[i] >= 0) { U[2 * i] = x[2 * freeIdx[i]]; U[2 * i + 1] = x[2 * freeIdx[i] + 1]; }
  }
  return { local, U };
}

// connected pieces of a set of triangles (through welded edges)
function components(topo, tris, inSet) {
  const seen = new Uint8Array(topo.T), out = [];
  for (const t0 of tris) {
    if (seen[t0]) continue;
    const comp = [t0];
    seen[t0] = 1;
    for (let i = 0; i < comp.length; i++) {
      const t = comp[i];
      for (let k = 0; k < 3; k++) {
        const a = topo.canon[3 * t + k], b = topo.canon[3 * t + ((k + 1) % 3)];
        const list = topo.edges.get(a < b ? a * topo.V + b : b * topo.V + a);
        if (!list) continue;
        for (const g of list) if (inSet(g) && !seen[g]) { seen[g] = 1; comp.push(g); }
      }
    }
    out.push(comp);
  }
  return out;
}

function closedChart(topo, tris, inSet) {
  for (const t of tris)
    for (let k = 0; k < 3; k++) {
      const a = topo.canon[3 * t + k], b = topo.canon[3 * t + ((k + 1) % 3)];
      const list = topo.edges.get(a < b ? a * topo.V + b : b * topo.V + a) || [];
      let n = 0;
      for (const g of list) if (inSet(g)) n++;
      if (n < 2) return false;
    }
  return true;
}

// Unwraps a region into charts: its connected pieces; a closed piece (no border: a whole sphere)
// is cut by the main direction its triangles face first. Writes UVs into `uv`; returns the charts.
export function unwrapRegion(topo, uv, tris) {
  const inR = new Uint8Array(topo.T);
  for (const t of tris) inR[t] = 1;
  let charts = [];
  for (const comp of components(topo, tris, (g) => inR[g])) {
    const inC = new Uint8Array(topo.T);
    for (const t of comp) inC[t] = 1;
    if (comp.length > 1 && closedChart(topo, comp, (g) => inC[g])) {
      const label = new Int8Array(topo.T);
      for (const t of comp) {
        const nx = topo.normal[3 * t], ny = topo.normal[3 * t + 1], nz = topo.normal[3 * t + 2];
        const ax = Math.abs(nx), ay = Math.abs(ny), az = Math.abs(nz);
        label[t] = ax >= ay && ax >= az ? (nx > 0 ? 0 : 1) : ay >= az ? (ny > 0 ? 2 : 3) : nz > 0 ? 4 : 5;
      }
      for (let lb = 0; lb < 6; lb++) {
        const part = comp.filter((t) => label[t] === lb);
        if (part.length) charts.push(...components(topo, part, (g) => inC[g] && label[g] === lb));
      }
    } else {
      charts.push(comp);
    }
  }
  for (const chart of charts) {
    const { local, U } = lscm(topo, chart);
    for (const t of chart)
      for (let k = 0; k < 3; k++) {
        const i = local.get(topo.canon[3 * t + k]);
        uv[2 * (3 * t + k)] = U[2 * i];
        uv[2 * (3 * t + k) + 1] = U[2 * i + 1];
      }
    // mirrored (clockwise) charts are flipped
    let sa = 0;
    for (const t of chart) sa += triUVArea(uv, t);
    if (sa < 0) for (const t of chart) for (let k = 0; k < 3; k++) uv[2 * (3 * t + k)] = -uv[2 * (3 * t + k)];
  }
  return charts;
}

// ------------------------------------------------------------------ orientation and scale

function chartCorners(chart) {
  const out = [];
  for (const t of chart) out.push(3 * t, 3 * t + 1, 3 * t + 2);
  return out;
}

function centerOf(uv, corners) {
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  for (const c of corners) { x0 = Math.min(x0, uv[2 * c]); x1 = Math.max(x1, uv[2 * c]); y0 = Math.min(y0, uv[2 * c + 1]); y1 = Math.max(y1, uv[2 * c + 1]); }
  return { cx: (x0 + x1) / 2, cy: (y0 + y1) / 2, w: x1 - x0, h: y1 - y0, x0, y0, x1, y1 };
}

function rotateCorners(uv, corners, ang, cx, cy) {
  const c = Math.cos(ang), s = Math.sin(ang);
  for (const k of corners) {
    const x = uv[2 * k] - cx, y = uv[2 * k + 1] - cy;
    uv[2 * k] = cx + c * x - s * y;
    uv[2 * k + 1] = cy + s * x + c * y;
  }
}

function hull(pts) {
  pts = pts.slice().sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  const cr = (o, a, b) => (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
  const lo = [], hi = [];
  for (const p of pts) { while (lo.length >= 2 && cr(lo[lo.length - 2], lo[lo.length - 1], p) <= 0) lo.pop(); lo.push(p); }
  for (let i = pts.length - 1; i >= 0; i--) { const p = pts[i]; while (hi.length >= 2 && cr(hi[hi.length - 2], hi[hi.length - 1], p) <= 0) hi.pop(); hi.push(p); }
  return lo.slice(0, -1).concat(hi.slice(0, -1));
}

// orient: 'upright' (model +Y becomes +V; flat horizontal pieces fall back to the longest side),
// 'longest' (the smallest bounding box, its long side along U) or 'none'
export function orientChart(topo, uv, chart, orient) {
  if (orient === 'none') return;
  const corners = chartCorners(chart);
  const { cx, cy } = centerOf(uv, corners);
  if (orient === 'upright') {
    // the UV direction in which the surface goes up, area weighted
    let gx = 0, gy = 0;
    for (const t of chart) {
      const o = 6 * t;
      const du1 = uv[o + 2] - uv[o], dv1 = uv[o + 3] - uv[o + 1], du2 = uv[o + 4] - uv[o], dv2 = uv[o + 5] - uv[o + 1];
      const y0 = topo.pos[3 * topo.canon[3 * t] + 1], dy1 = topo.pos[3 * topo.canon[3 * t + 1] + 1] - y0, dy2 = topo.pos[3 * topo.canon[3 * t + 2] + 1] - y0;
      const det = du1 * dv2 - du2 * dv1;
      if (Math.abs(det) < 1e-20) continue;
      // grad of y in uv: [du1 dv1; du2 dv2] g = [dy1; dy2]
      const ggx = (dy1 * dv2 - dy2 * dv1) / det, ggy = (du1 * dy2 - du2 * dy1) / det;
      const w = topo.area[t];
      gx += ggx * w; gy += ggy * w;
    }
    // nearly horizontal: up has no meaningful direction
    let tilt = 0, tot = 0;
    for (const t of chart) { tilt += (1 - Math.abs(topo.normal[3 * t + 1])) * topo.area[t]; tot += topo.area[t]; }
    if (Math.hypot(gx, gy) > 1e-12 && tilt > 0.15 * tot) {
      rotateCorners(uv, corners, Math.PI / 2 - Math.atan2(gy, gx), cx, cy);
      return;
    }
  }
  // the minimum-area bounding box, laid along U
  const pts = corners.map((c) => [uv[2 * c], uv[2 * c + 1]]);
  const h = hull(pts);
  let best = Infinity, bestAng = 0;
  for (let i = 0; i < h.length; i++) {
    const a = h[i], b = h[(i + 1) % h.length];
    const ang = Math.atan2(b[1] - a[1], b[0] - a[0]);
    const c = Math.cos(-ang), s = Math.sin(-ang);
    let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
    for (const p of h) { const x = c * p[0] - s * p[1], y = s * p[0] + c * p[1]; x0 = Math.min(x0, x); x1 = Math.max(x1, x); y0 = Math.min(y0, y); y1 = Math.max(y1, y); }
    let area = (x1 - x0) * (y1 - y0), a2 = -ang;
    if (y1 - y0 > x1 - x0) a2 += Math.PI / 2;
    if (area < best * (1 - 1e-6)) { best = area; bestAng = a2; }
  }
  rotateCorners(uv, corners, bestAng, cx, cy);
}

export function scaleChart(topo, uv, chart, target) {
  const d = density(topo, uv, chart);
  if (!(d > 0) || !(target > 0)) return;
  const s = target / d;
  const corners = chartCorners(chart);
  const { cx, cy } = centerOf(uv, corners);
  for (const c of corners) { uv[2 * c] = cx + (uv[2 * c] - cx) * s; uv[2 * c + 1] = cy + (uv[2 * c + 1] - cy) * s; }
}

// density targets (UV units per model unit), the same rules as the desktop app:
//  'fit'    the surface that needs texels of its own (mirror copies share theirs) fills 70% of 0..1
//  'layout' the area-weighted median density of the existing islands in 0..1 (without `exclude`
//           and mirror copies), so neither one odd island nor many small ones skew it
//  'auto'   the layout, at most twice 'fit' (a denser layout overlaps heavily), else 'fit'
export function densityTargets(topo, uv, flags, exclude) {
  let total = 0;
  for (let t = 0; t < topo.T; t++) if (!(flags[t] & F_COPY)) total += topo.area[t];
  const fit = total > 0 ? Math.sqrt(0.7 / total) : 1;
  const isl = uvIslands(topo, uv);
  const ds = [];
  let wsum = 0;
  for (const l of islandLists(isl)) {
    const keep = l.filter((t) => !(exclude && exclude[t]) && !(flags[t] & F_COPY));
    if (!keep.length) continue;
    let a3 = 0, all = 0, inside = 0;
    for (const t of keep) {
      const u = Math.abs(triUVArea(uv, t)), o = 6 * t;
      const cu = (uv[o] + uv[o + 2] + uv[o + 4]) / 3, cv = (uv[o + 1] + uv[o + 3] + uv[o + 5]) / 3;
      a3 += topo.area[t]; all += u;
      if (cu >= 0 && cv >= 0 && cu <= 1 && cv <= 1) inside += u;
    }
    if (!(all > 0) || inside < all * 0.5) continue;  // tiled or parked outside the texture
    const d = density(topo, uv, keep);
    if (d > 1e-6) { ds.push([d, a3]); wsum += a3; }
  }
  ds.sort((a, b) => a[0] - b[0]);
  let layout = 0, acc = 0;
  for (const [d, a] of ds) { acc += a; if (acc >= wsum * 0.5) { layout = d; break; } }
  return { fit, layout, auto: layout > 0 ? Math.min(layout, 2 * fit) : fit };
}

// ------------------------------------------------------------------ free-space placement

const G = 512;

function rasterTri(uv, t, grid, ox = 0, oy = 0, value = 1) {
  const o = 6 * t;
  const ax = (uv[o] + ox) * G, ay = (uv[o + 1] + oy) * G, bx = (uv[o + 2] + ox) * G, by = (uv[o + 3] + oy) * G, cx = (uv[o + 4] + ox) * G, cy = (uv[o + 5] + oy) * G;
  const x0 = Math.max(0, Math.floor(Math.min(ax, bx, cx))), x1 = Math.min(G - 1, Math.floor(Math.max(ax, bx, cx)));
  const y0 = Math.max(0, Math.floor(Math.min(ay, by, cy))), y1 = Math.min(G - 1, Math.floor(Math.max(ay, by, cy)));
  // the cell of the center always (tiny triangles)
  const mx = Math.floor((ax + bx + cx) / 3), my = Math.floor((ay + by + cy) / 3);
  if (mx >= 0 && mx < G && my >= 0 && my < G) grid[my * G + mx] = value;
  const e = (px, py, qx, qy, x, y) => (qx - px) * (y - py) - (qy - py) * (x - px);
  const area = e(ax, ay, bx, by, cx, cy);
  if (Math.abs(area) < 1e-12) return;
  for (let y = y0; y <= y1; y++)
    for (let x = x0; x <= x1; x++) {
      const px = x + 0.5, py = y + 0.5;
      const w0 = e(bx, by, cx, cy, px, py), w1 = e(cx, cy, ax, ay, px, py), w2 = e(ax, ay, bx, by, px, py);
      if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) grid[y * G + x] = value;
    }
}

function dilate(grid, r) {
  for (let pass = 0; pass < r; pass++) {
    const src = grid.slice();
    for (let y = 0; y < G; y++)
      for (let x = 0; x < G; x++) {
        if (src[y * G + x]) continue;
        if ((x > 0 && src[y * G + x - 1]) || (x < G - 1 && src[y * G + x + 1]) || (y > 0 && src[(y - 1) * G + x]) || (y < G - 1 && src[(y + 1) * G + x]))
          grid[y * G + x] = 1;
      }
  }
}

// Moves the charts into empty parts of 0..1 (largest first, rotated 90 degrees when that helps and
// `rotate` allows), leaving every other triangle where it is. Mirror copies take no space (they
// sit on the triangles they copy). Returns false if a chart found no room (left unchanged).
export function placeInFreeSpace(uv, flags, T, charts, margin = 0.005, rotate = false, obstacle = null) {
  const moving = new Uint8Array(T);
  for (const c of charts) for (const t of c) moving[t] = 1;
  const occ = new Uint8Array(G * G);
  for (let t = 0; t < T; t++) {
    if (moving[t] || (flags[t] & F_COPY)) continue;
    if (obstacle && !obstacle[t]) continue;
    rasterTri(uv, t, occ);
  }
  const mc = Math.max(1, Math.ceil(margin * G));
  dilate(occ, mc);
  const order = charts.map((c, i) => [i, c.reduce((s, t) => s + Math.abs(triUVArea(uv, t)), 0)]).sort((a, b) => b[1] - a[1]);
  let all = true;
  for (const [ci] of order) {
    const chart = charts[ci], corners = chartCorners(chart);
    let placed = false;
    for (let rot = 0; rot < (rotate ? 2 : 1) && !placed; rot++) {
      const keep = corners.map((c) => [uv[2 * c], uv[2 * c + 1]]);
      const b0 = centerOf(uv, corners);
      if (rot) rotateCorners(uv, corners, Math.PI / 2, b0.cx, b0.cy);
      const b = centerOf(uv, corners);
      const w = Math.ceil(b.w * G) + 1, h = Math.ceil(b.h * G) + 1;
      const restore = () => corners.forEach((c, i) => { uv[2 * c] = keep[i][0]; uv[2 * c + 1] = keep[i][1]; });
      if (!(w <= G && h <= G) || !isFinite(b.w)) { restore(); continue; }
      const fp = new Uint8Array(G * G);
      for (const t of chart) rasterTri(uv, t, fp, -b.x0 + 0.5 / G, -b.y0 + 0.5 / G);
      const cells = [];
      for (let j = 0; j < h && j < G; j++) for (let i = 0; i < w && i < G; i++) if (fp[j * G + i]) cells.push(i, j);
      // summed-area table of the occupancy
      const S = new Int32Array((G + 1) * (G + 1));
      for (let y = 0; y < G; y++) {
        let row = 0;
        for (let x = 0; x < G; x++) { row += occ[y * G + x]; S[(y + 1) * (G + 1) + x + 1] = S[y * (G + 1) + x + 1] + row; }
      }
      const sum = (x0, y0, x1, y1) => S[(y1 + 1) * (G + 1) + x1 + 1] - S[y0 * (G + 1) + x1 + 1] - S[(y1 + 1) * (G + 1) + x0] + S[y0 * (G + 1) + x0];
      let bx = -1, by = -1;
      for (let y = 0; y + h <= G && bx < 0; y++)
        for (let x = 0; x + w <= G; x++) if (sum(x, y, x + w - 1, y + h - 1) === 0) { bx = x; by = y; break; }
      if (bx < 0)
        for (let y = 0; y + h <= G && bx < 0; y += 2)
          for (let x = 0; x + w <= G; x += 2) {
            let free = true;
            for (let k = 0; k < cells.length; k += 2) if (occ[(y + cells[k + 1]) * G + x + cells[k]]) { free = false; break; }
            if (free) { bx = x; by = y; break; }
          }
      if (bx < 0) { restore(); continue; }
      const dx = bx / G - b.x0 + 0.5 / G, dy = by / G - b.y0 + 0.5 / G;
      for (const c of corners) { uv[2 * c] += dx; uv[2 * c + 1] += dy; }
      const add = new Uint8Array(G * G);
      for (const t of chart) rasterTri(uv, t, add);
      dilate(add, mc);
      for (let k = 0; k < occ.length; k++) occ[k] |= add[k];
      placed = true;
    }
    if (!placed) all = false;
  }
  return all;
}

// Repacks every island that may move (not pinned, not a mirror copy) at one density into the space
// the pinned islands leave, as large as it fits. `extra`: charts to include that are not islands
// yet (a region just unwrapped). Returns the scale used relative to `target`, or 0.
export function repackMovable(topo, uv, flags, target, extra = [], margin = 0.005) {
  const isl = uvIslands(topo, uv);
  const inExtra = new Uint8Array(topo.T);
  for (const c of extra) for (const t of c) inExtra[t] = 1;
  const charts = extra.slice();
  for (const l of islandLists(isl)) {
    const keep = l.filter((t) => !inExtra[t]);
    if (!keep.length || keep.some((t) => flags[t] & F_PIN) || keep.every((t) => flags[t] & F_COPY)) continue;
    charts.push(keep);
  }
  if (!charts.length) return 0;
  const pinned = new Uint8Array(topo.T);
  let fixedUV = 0, area3 = 0;
  for (let t = 0; t < topo.T; t++) if (flags[t] & F_PIN && !(flags[t] & F_COPY)) { pinned[t] = 1; fixedUV += Math.abs(triUVArea(uv, t)); }
  // by shape (islands nest into each other's gaps), at most at the target density
  {
    const k = packShapes(topo, uv, charts, { margin, rotate: true, maxScale: target, obstacle: (t) => pinned[t] });
    if (k > 0) return k / target;
  }
  for (const c of charts) for (const t of c) area3 += topo.area[t];
  const free = Math.max(0.05, 1 - fixedUV);
  let k = Math.min(1, Math.sqrt((0.72 * free) / Math.max(area3 * target * target, 1e-30)));
  const corners = [];
  for (const c of charts) corners.push(...chartCorners(c));
  const keep = corners.map((c) => [uv[2 * c], uv[2 * c + 1]]);
  for (let attempt = 0; attempt < 40; attempt++, k *= 0.92) {
    corners.forEach((c, i) => { uv[2 * c] = keep[i][0]; uv[2 * c + 1] = keep[i][1]; });
    for (const c of charts) scaleChart(topo, uv, c, target * k);
    if (placeInFreeSpace(uv, flags, topo.T, charts, margin, true, pinned)) return k;
  }
  corners.forEach((c, i) => { uv[2 * c] = keep[i][0]; uv[2 * c + 1] = keep[i][1]; });
  return 0;
}

// ------------------------------------------------------------------ the Island brush

// Makes the selected triangles UV islands (one per connected piece): unwrapped (LSCM), oriented,
// scaled to the target density and placed in free UV space without moving other islands
// (repacking the unpinned ones only when there is no room). With mirror axes, the triangles on the
// negative side of the axes take the mirrored UVs of the positive side: both halves share one place.
// opts: { orient: 'upright'|'longest'|'none', density: number (UV units per model unit),
//         axes: bitmask, pin: bool, margin }
// Modifies uv and flags; returns { ok, charts, stacked, repacked, message, pinnedLeft }.
export function makeIsland(topo, uv, flags, selected, opts) {
  const margin = opts.margin ?? 0.005;
  let sel = selected.filter((t) => !(flags[t] & F_PIN));
  const pinnedLeft = selected.length - sel.length;
  if (!sel.length) return { ok: false, message: 'Those triangles are pinned: unpin them first (UV view, Alt+P)', pinnedLeft };
  const axes = opts.axes & 7;
  const inSel = new Uint8Array(topo.T);
  for (const t of sel) inSel[t] = 1;
  if (axes) {
    mirrorSelection(topo, inSel, axes);
    for (let t = 0; t < topo.T; t++) if (inSel[t] && flags[t] & F_PIN) inSel[t] = 0;
    sel = [];
    for (let t = 0; t < topo.T; t++) if (inSel[t]) sel.push(t);
  }
  // copies: triangles on a negative side whose mirrored corners are all on selected positive triangles
  const copies = [], primary = [], source = new Map();
  if (axes) {
    const positiveSel = (g) => inSel[g] && !negMask(topo, g, axes);
    for (const t of sel) {
      const neg = negMask(topo, t, axes);
      const s = neg ? mirrorCorners(topo, t, neg, positiveSel) : null;
      if (s) { copies.push(t); source.set(t, s); } else primary.push(t);
    }
  } else primary.push(...sel);
  if (!primary.length) return { ok: false, message: 'Nothing to unwrap', pinnedLeft };
  for (const t of sel) flags[t] &= ~F_MIRROR;
  const target = opts.density;
  const charts = unwrapRegion(topo, uv, primary);
  for (const c of charts) { orientChart(topo, uv, c, opts.orient); scaleChart(topo, uv, c, target); }
  // the copies' old UVs must not take space while the primary half is placed
  for (const t of copies) flags[t] |= F_COPY;
  let fits = placeInFreeSpace(uv, flags, topo.T, charts, margin, opts.orient !== 'upright');
  if (!fits && opts.orient === 'upright') fits = placeInFreeSpace(uv, flags, topo.T, charts, margin, true);
  let repacked = false;
  if (!fits) {
    const k = repackMovable(topo, uv, flags, target, charts, margin);
    if (!k) {
      for (const t of copies) flags[t] &= ~F_COPY;
      return { ok: false, message: 'No room in the UV layout, even after repacking', pinnedLeft };
    }
    repacked = true;
  }
  for (const t of copies) {
    const s = source.get(t), neg = negMask(topo, t, axes);
    for (let k = 0; k < 3; k++) { uv[2 * (3 * t + k)] = uv[2 * s[k]]; uv[2 * (3 * t + k) + 1] = uv[2 * s[k] + 1]; }
    flags[t] = (flags[t] & ~F_MIRROR) | F_COPY | (neg << 1);
  }
  if (opts.pin) for (const t of sel) flags[t] |= F_PIN;
  return { ok: true, charts: charts.length, stacked: copies.length, repacked, pinnedLeft, message: '' };
}

// ------------------------------------------------------------------ curvature (smart masks)

// Per-corner convexity 0..1 (0.5 flat, above: convex edges and ridges, below: cavities). At each
// welded vertex: the mean sine of the angle its edges make with its tangent plane, scaled by the
// 95th percentile over the mesh, then smoothed once over the neighbors. Independent of the UVs
// and of how the mesh is tessellated (a ratio of lengths).
export function vertexCurvature(topo) {
  const V = topo.V, T = topo.T;
  const n = new Float64Array(3 * V);
  for (let t = 0; t < T; t++)
    for (let k = 0; k < 3; k++) {
      const v = topo.canon[3 * t + k];
      for (let a = 0; a < 3; a++) n[3 * v + a] += topo.normal[3 * t + a] * topo.area[t];
    }
  for (let v = 0; v < V; v++) {
    const l = Math.hypot(n[3 * v], n[3 * v + 1], n[3 * v + 2]) || 1;
    for (let a = 0; a < 3; a++) n[3 * v + a] /= l;
  }
  const sum = new Float64Array(V), cnt = new Float64Array(V);
  const nb = Array.from({ length: V }, () => []);
  for (const key of topo.edges.keys()) {
    const a = Math.floor(key / V), b = key - a * V;
    nb[a].push(b); nb[b].push(a);
    const dx = topo.pos[3 * b] - topo.pos[3 * a], dy = topo.pos[3 * b + 1] - topo.pos[3 * a + 1], dz = topo.pos[3 * b + 2] - topo.pos[3 * a + 2];
    const l = Math.hypot(dx, dy, dz);
    if (!(l > 0)) continue;
    sum[a] += -(n[3 * a] * dx + n[3 * a + 1] * dy + n[3 * a + 2] * dz) / l; cnt[a]++;
    sum[b] += (n[3 * b] * dx + n[3 * b + 1] * dy + n[3 * b + 2] * dz) / l; cnt[b]++;
  }
  let c = new Float64Array(V);
  for (let v = 0; v < V; v++) c[v] = cnt[v] ? sum[v] / cnt[v] : 0;
  const s = new Float64Array(V);
  for (let v = 0; v < V; v++) {
    let acc = c[v] * 2, w = 2;
    for (const u of nb[v]) { acc += c[u]; w++; }
    s[v] = acc / w;
  }
  c = s;
  const mags = Array.from(c, Math.abs).sort((x, y) => x - y);
  const scale = Math.max(mags[Math.floor(mags.length * 0.95)] || 0, 1e-4);
  const out = new Float32Array(3 * T);
  for (let i = 0; i < 3 * T; i++) out[i] = Math.min(1, Math.max(0, 0.5 + 0.5 * c[topo.canon[i]] / scale));
  return out;
}

// ------------------------------------------------------------------ relax

// Relaxes the UV islands of `tris`: each is solved again conformally (LSCM over its own triangles,
// with corners that share a position but not a UV kept apart, so islands cut open stay open) and
// fitted back onto where it was: same center, rotation (least squares) and area. Pinned islands
// and mirror copies are left alone (copies follow their other half). Returns the islands relaxed.
export function relaxIslands(topo, uv, flags, tris) {
  const isl = uvIslands(topo, uv), lists = islandLists(isl);
  const want = new Set(tris.map((t) => isl.id[t]));
  let done = 0;
  for (const id of want) {
    const chart = lists[id];
    if (chart.some((t) => flags[t] & F_PIN) || chart.every((t) => flags[t] & F_COPY)) continue;
    // UV vertices: welded position + UV
    const key = new Map(), canon = new Int32Array(3 * topo.T), pos = [];
    for (const t of chart)
      for (let k = 0; k < 3; k++) {
        const c = 3 * t + k, v = topo.canon[c];
        const kk = v + ':' + Math.round(uv[2 * c] * 1e6) + ':' + Math.round(uv[2 * c + 1] * 1e6);
        let i = key.get(kk);
        if (i === undefined) { i = pos.length; key.set(kk, i); pos.push(topo.p(v)); }
        canon[c] = i;
      }
    const proxy = { canon, p: (i) => pos[i] };
    const { local, U } = lscm(proxy, chart);
    const corners = chartCorners(chart);
    const before = corners.map((c) => [uv[2 * c], uv[2 * c + 1]]);
    const after = corners.map((c) => { const i = local.get(canon[c]); return [U[2 * i], U[2 * i + 1]]; });
    let a0 = 0, a1 = 0;
    for (let j = 0; j < chart.length; j++) {
      const t = (k) => before[3 * j + k], u = (k) => after[3 * j + k];
      a0 += ((t(1)[0] - t(0)[0]) * (t(2)[1] - t(0)[1]) - (t(2)[0] - t(0)[0]) * (t(1)[1] - t(0)[1])) / 2;
      a1 += ((u(1)[0] - u(0)[0]) * (u(2)[1] - u(0)[1]) - (u(2)[0] - u(0)[0]) * (u(1)[1] - u(0)[1])) / 2;
    }
    if (!(Math.abs(a1) > 1e-20) || !isFinite(a1)) continue;
    if ((a1 < 0) !== (a0 < 0)) for (const q of after) q[0] = -q[0];
    const cb = [0, 0], ca = [0, 0];
    for (let i = 0; i < corners.length; i++) { cb[0] += before[i][0]; cb[1] += before[i][1]; ca[0] += after[i][0]; ca[1] += after[i][1]; }
    cb[0] /= corners.length; cb[1] /= corners.length; ca[0] /= corners.length; ca[1] /= corners.length;
    let sa = 0, sb = 0;
    for (let i = 0; i < corners.length; i++) {
      const qx = after[i][0] - ca[0], qy = after[i][1] - ca[1], px = before[i][0] - cb[0], py = before[i][1] - cb[1];
      sa += qx * px + qy * py;
      sb += qx * py - qy * px;
    }
    const ang = Math.atan2(sb, sa), co = Math.cos(ang), si = Math.sin(ang), s = Math.sqrt(Math.abs(a0) / Math.abs(a1));
    for (let i = 0; i < corners.length; i++) {
      const qx = after[i][0] - ca[0], qy = after[i][1] - ca[1];
      uv[2 * corners[i]] = cb[0] + s * (co * qx - si * qy);
      uv[2 * corners[i] + 1] = cb[1] + s * (si * qx + co * qy);
    }
    done++;
  }
  return done;
}

// ------------------------------------------------------------------ packing by shape
// The desktop app's packer (src/core/UVPack.cpp) in JavaScript: island outlines rasterized as runs
// of cells per row; each island, largest first, goes to the candidate place (the leftmost fit in
// the lowest rows) where its outline touches the most, at 4 rotations; the scale is bisected.

function addTriRuns(p0, p1, p2, rows, nrows, xmin, xmax) {
  const v = [p0, p1, p2];
  const ylo = Math.min(p0[1], p1[1], p2[1]), yhi = Math.max(p0[1], p1[1], p2[1]);
  let j0 = Math.max(0, Math.floor(ylo)), j1 = Math.min(nrows - 1, Math.ceil(yhi) - 1);
  if (Math.ceil(yhi) - 1 < Math.floor(ylo)) j1 = Math.min(nrows - 1, Math.floor(ylo));
  for (let j = j0; j <= j1; j++) {
    let lo = Infinity, hi = -Infinity;
    for (let k = 0; k < 3; k++) {
      const a = v[k], b = v[(k + 1) % 3];
      if (a[1] >= j && a[1] <= j + 1) { lo = Math.min(lo, a[0]); hi = Math.max(hi, a[0]); }
      for (const yy of [j, j + 1])
        if ((a[1] - yy) * (b[1] - yy) < 0) {
          const x = a[0] + ((yy - a[1]) / (b[1] - a[1])) * (b[0] - a[0]);
          lo = Math.min(lo, x); hi = Math.max(hi, x);
        }
    }
    if (lo > hi) continue;
    let a = Math.floor(lo), b = Math.max(a, Math.ceil(hi) - 1);
    a = Math.max(a, xmin); b = Math.min(b, xmax);
    if (a <= b) rows[j].push([a, b]);
  }
}
function mergeRuns(r) {
  if (r.length < 2) return r;
  r.sort((x, y) => x[0] - y[0]);
  const out = [r[0].slice()];
  for (let i = 1; i < r.length; i++) {
    const l = out[out.length - 1];
    if (r[i][0] <= l[1] + 1) l[1] = Math.max(l[1], r[i][1]); else out.push(r[i].slice());
  }
  return out;
}
class Occ {
  constructor(R) { this.R = R; this.cell = new Uint8Array(R * R); this.pre = new Int32Array(R * (R + 1)); this.longest = new Int32Array(R).fill(R); }
  rebuild(y) {
    const R = this.R, o = y * (R + 1), c = y * R;
    let run = 0, best = 0;
    this.pre[o] = 0;
    for (let x = 0; x < R; x++) {
      this.pre[o + x + 1] = this.pre[o + x] + this.cell[c + x];
      run = this.cell[c + x] ? 0 : run + 1;
      if (run > best) best = run;
    }
    this.longest[y] = best;
  }
  free(y, a, b) { const o = y * (this.R + 1); return this.pre[o + b + 1] === this.pre[o + a]; }
  lastOccupied(y, a, b) {
    const o = y * (this.R + 1), target = this.pre[o + b + 1];
    let lo = a + 1, hi = b + 1;
    while (lo < hi) { const mid = (lo + hi) >> 1; if (this.pre[o + mid] === target) hi = mid; else lo = mid + 1; }
    return lo - 1;
  }
  occupied(y, a, b) {
    const R = this.R;
    if (y < 0 || y >= R) return b - a + 1;
    let n = 0;
    if (a < 0) { n += -a; a = 0; }
    if (b >= R) { n += b - R + 1; b = R - 1; }
    if (a <= b) n += this.pre[y * (R + 1) + b + 1] - this.pre[y * (R + 1) + a];
    return n;
  }
  mark(y, a, b) {
    if (y < 0 || y >= this.R) return;
    a = Math.max(a, 0); b = Math.min(b, this.R - 1);
    this.cell.fill(1, y * this.R + a, y * this.R + b + 1);
  }
  clone() { const o = new Occ(this.R); o.cell.set(this.cell); o.pre.set(this.pre); o.longest.set(this.longest); return o; }
}
const rot90 = (p, r) => (r & 3) === 1 ? [-p[1], p[0]] : (r & 3) === 2 ? [-p[0], -p[1]] : (r & 3) === 3 ? [p[1], -p[0]] : p;

// charts: arrays of triangles. obstacle(t): the triangle's UVs stay and take space. maxScale (> 0):
// the largest density (UV units per model unit). Returns the scale, or 0 (UVs unchanged) if nothing fits.
export function packShapes(topo, uv, charts, { margin = 0.005, rotate = true, maxScale = 0, obstacle = null, resolution = 512 } = {}) {
  const R = resolution;
  const shapes = [];
  for (const chart of charts) {
    let a3 = 0, au = 0;
    for (const t of chart) { a3 += topo.area[t]; au += Math.abs(triUVArea(uv, t)); }
    const sc = a3 > 1e-30 && au > 1e-30 ? Math.sqrt(a3 / au) : 1;
    const corners = chartCorners(chart);
    let pts = corners.map((c) => [uv[2 * c] * sc, uv[2 * c + 1] * sc]);
    if (rotate) {
      // smallest bounding box
      const h = hull(pts);
      let best = Infinity, bc = 1, bs = 0;
      const boxArea = (c, s) => { let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity; for (const q of h) { const x = c * q[0] + s * q[1], y = -s * q[0] + c * q[1]; x0 = Math.min(x0, x); x1 = Math.max(x1, x); y0 = Math.min(y0, y); y1 = Math.max(y1, y); } return (x1 - x0) * (y1 - y0); };
      best = boxArea(1, 0);
      for (let i = 0; i < h.length; i++) {
        const d = [h[(i + 1) % h.length][0] - h[i][0], h[(i + 1) % h.length][1] - h[i][1]], l = Math.hypot(d[0], d[1]);
        if (l < 1e-300) continue;
        const ar = boxArea(d[0] / l, d[1] / l);
        if (ar < best * (1 - 1e-6)) { best = ar; bc = d[0] / l; bs = d[1] / l; }
      }
      pts = pts.map((q) => [bc * q[0] + bs * q[1], -bs * q[0] + bc * q[1]]);
    }
    let area = 0;
    for (let i = 0; i < chart.length; i++) {
      const a = pts[3 * i], b = pts[3 * i + 1], c = pts[3 * i + 2];
      area += Math.abs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])) / 2;
    }
    const lo = [], size = [];
    for (let r = 0; r < 4; r++) {
      let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
      for (const q of pts) { const p = rot90(q, r); x0 = Math.min(x0, p[0]); y0 = Math.min(y0, p[1]); x1 = Math.max(x1, p[0]); y1 = Math.max(y1, p[1]); }
      lo.push([x0, y0]); size.push([x1 - x0, y1 - y0]);
    }
    shapes.push({ corners, pts, area, lo, size, n: chart.length });
  }
  if (!shapes.length) return 0;
  const order = shapes.map((_, i) => i).sort((a, b) => {
    const ma = Math.max(...shapes[a].size[0]), mb = Math.max(...shapes[b].size[0]);
    return (shapes[b].area + mb * mb) - (shapes[a].area + ma * ma);
  });
  const mc = Math.max(1, Math.ceil(margin * R)), border = Math.max(1, mc >> 1);
  const base = new Occ(R);
  if (obstacle) {
    const rows = Array.from({ length: R }, () => []);
    for (let t = 0; t < topo.T; t++) {
      if (!obstacle(t)) continue;
      const o = 6 * t;
      addTriRuns([uv[o] * R, uv[o + 1] * R], [uv[o + 2] * R, uv[o + 3] * R], [uv[o + 4] * R, uv[o + 5] * R], rows, R, -mc, R - 1 + mc);
    }
    for (let y = 0; y < R; y++) for (const [a, b] of rows[y]) for (let yy = y - mc; yy <= y + mc; yy++) base.mark(yy, a - mc, b + mc);
  }
  for (let y = 0; y < R; y++)
    for (let x = 0; x < R; x++) if (y < border || y >= R - border || x < border || x >= R - border) base.cell[y * R + x] = 1;
  for (let y = 0; y < R; y++) base.rebuild(y);
  const footprint = (s, rot, k) => {
    const sc = k * R, lo = [s.lo[rot][0] * sc, s.lo[rot][1] * sc];
    const w = Math.max(1, Math.ceil(s.size[rot][0] * sc - 1e-9)), h = Math.max(1, Math.ceil(s.size[rot][1] * sc - 1e-9));
    const rows = Array.from({ length: h }, () => []);
    for (let i = 0; i < s.n; i++) {
      const q = [0, 1, 2].map((k2) => { const p = rot90(s.pts[3 * i + k2], rot); return [p[0] * sc - lo[0], p[1] * sc - lo[1]]; });
      addTriRuns(q[0], q[1], q[2], rows, h, 0, w - 1);
    }
    let wideRow = 0, wideRun = 1;
    const width = rows.map((r, j) => { const m = mergeRuns(r); rows[j] = m; let n = 0; for (const [a, b] of m) { n += b - a + 1; if (b - a + 1 > wideRun) { wideRun = b - a + 1; wideRow = j; } } return n; });
    const ord = rows.map((_, j) => j).sort((a, b) => width[b] - width[a]);
    return { w, h, rows, ord, wideRow, wideRun };
  };
  const places = (occ, f, max) => {
    const out = [];
    if (f.w > R || f.h > R) return out;
    let firstY = -1;
    for (let y = 0; y + f.h <= R; y++) {
      if (firstY >= 0 && (y - firstY > f.h * 2 + 4 || out.length >= max)) break;
      if (occ.longest[y + f.wideRow] < f.wideRun) continue;
      let x = 0;
      while (x + f.w <= R) {
        let next = -1;
        for (const j of f.ord) {
          for (const [a, b] of f.rows[j])
            if (!occ.free(y + j, x + a, x + b)) { next = occ.lastOccupied(y + j, x + a, x + b) - a + 1; break; }
          if (next >= 0) break;
        }
        if (next < 0) { out.push([x, y]); if (firstY < 0) firstY = y; break; }
        x = Math.max(x + 1, next);
      }
    }
    return out;
  };
  const contact = (occ, f, x, y) => {
    let touch = 0, total = 0;
    for (let j = 0; j < f.h; j++)
      for (const [a0, b0] of f.rows[j]) {
        const a = x + a0, b = x + b0, yy = y + j;
        touch += occ.occupied(yy, a - 1, a - 1) + occ.occupied(yy, b + 1, b + 1) + occ.occupied(yy - 1, a, b) + occ.occupied(yy + 1, a, b);
        total += 2 + 2 * (b0 - a0 + 1);
      }
    return total ? touch / total : 0;
  };
  const sameHalfTurn = (a, b) => {
    if (a.w !== b.w || a.h !== b.h) return false;
    for (let j = 0; j < a.h; j++) {
      const ra = a.rows[j], rb = b.rows[a.h - 1 - j];
      if (ra.length !== rb.length) return false;
      for (let i = 0; i < ra.length; i++) { const p = rb[rb.length - 1 - i]; if (ra[i][0] !== a.w - 1 - p[1] || ra[i][1] !== a.w - 1 - p[0]) return false; }
    }
    return true;
  };
  const rots = rotate ? 4 : 1;
  const tryPack = (k) => {
    const occ = base.clone(), place = new Array(shapes.length);
    for (const i of order) {
      let best = null;
      const fps = [];
      for (let r = 0; r < rots; r++) {
        fps[r] = footprint(shapes[i], r, k);
        if (r >= 2 && sameHalfTurn(fps[r - 2], fps[r])) continue;
        for (const [x, y] of places(occ, fps[r], 8)) {
          const sc = contact(occ, fps[r], x, y) - 1.5 * (y + fps[r].h) / R - 0.25 * x / R;
          if (!best || sc > best.sc) best = { sc, r, x, y, f: fps[r] };
        }
      }
      if (!best) return null;
      for (let j = 0; j < best.f.h; j++)
        for (const [a, b] of best.f.rows[j]) for (let yy = best.y + j - mc; yy <= best.y + j + mc; yy++) occ.mark(yy, best.x + a - mc, best.x + b + mc);
      for (let yy = Math.max(0, best.y - mc); yy <= Math.min(R - 1, best.y + best.f.h - 1 + mc); yy++) occ.rebuild(yy);
      place[i] = best;
    }
    return place;
  };
  let total = 0;
  for (const s of shapes) total += s.area;
  let free = 0;
  for (let i = 0; i < R * R; i++) free += base.cell[i] ? 0 : 1;
  let hi = total > 1e-30 ? Math.sqrt(Math.max(0.02, free / (R * R)) / total) : 1;
  for (const s of shapes) { const side = Math.max(...s.size[0]); if (side > 1e-30) hi = Math.min(hi, (1 - 2 * (border + 1) / R) / side); }
  if (maxScale > 0) hi = Math.min(hi, maxScale);
  let bestK = 0, best = null, lo = 0;
  let pl = tryPack(hi);
  if (pl) { bestK = hi; best = pl; } else {
    let k = hi;
    for (let g = 0; g < 30 && !best; g++) { k *= 0.85; pl = tryPack(k); if (pl) { lo = k; bestK = k; best = pl; } else hi = k; }
    if (!best) return 0;
    for (let it = 0; it < 9 && hi > lo * 1.01; it++) {
      const mid = Math.sqrt(lo * hi);
      pl = tryPack(mid);
      if (pl) { lo = mid; bestK = mid; best = pl; } else hi = mid;
    }
  }
  shapes.forEach((s, i) => {
    const p = best[i], lo2 = [s.lo[p.r][0] * bestK, s.lo[p.r][1] * bestK];
    s.corners.forEach((c, j) => {
      const q = rot90(s.pts[j], p.r);
      uv[2 * c] = q[0] * bestK - lo2[0] + p.x / R;
      uv[2 * c + 1] = q[1] * bestK - lo2[1] + p.y / R;
    });
  });
  return bestK;
}
