// Model preparation: merge meshes, normalize size, check / generate UVs.
import * as THREE from 'three';
import { mergeGeometries } from 'three/addons/utils/BufferGeometryUtils.js';

// Collects every mesh under `root` into one non-indexed geometry (world transforms
// applied) with position / normal / uv. Returns { geometry, hadUVs }.
export function mergeScene(root) {
  root.updateMatrixWorld(true);
  const parts = [];
  let hadUVs = true;
  root.traverse((o) => {
    if (!o.isMesh || !o.geometry || !o.geometry.attributes.position) return;
    let g = o.geometry.clone();
    if (g.index) g = g.toNonIndexed();
    g.applyMatrix4(o.matrixWorld);
    for (const k of Object.keys(g.attributes)) if (!['position', 'normal', 'uv'].includes(k)) g.deleteAttribute(k);  // (tangents are recomputed)
    if (!g.attributes.normal) g.computeVertexNormals();
    if (!g.attributes.uv) {
      hadUVs = false;
      g.setAttribute('uv', new THREE.BufferAttribute(new Float32Array((g.attributes.position.count) * 2), 2));
    }
    g.morphAttributes = {};
    parts.push(g);
  });
  if (!parts.length) throw new Error('no meshes in this file');
  const geometry = parts.length === 1 ? parts[0] : mergeGeometries(parts, false);
  return { geometry, hadUVs };
}

// Centers the geometry and scales it to a bounding radius of 1 (materials are
// projected in object space, so their scale is the same on every model).
// Returns the matrix that undoes it (for exporting at the original size).
export function normalize(geometry) {
  geometry.computeBoundingSphere();
  const s = geometry.boundingSphere;
  const k = s.radius > 0 ? 1 / s.radius : 1;
  const m = new THREE.Matrix4().makeScale(k, k, k).multiply(new THREE.Matrix4().makeTranslation(-s.center.x, -s.center.y, -s.center.z));
  geometry.applyMatrix4(m);
  geometry.computeBoundingSphere();
  geometry.computeBoundingBox();
  return m.clone().invert();
}

// Rough UV health check on a coarse grid: fraction of covered cells that more than
// one triangle claims (mirrored / stacked islands), and whether UVs leave 0..1.
export function uvReport(geometry, res = 256) {
  const uv = geometry.attributes.uv.array;
  const n = uv.length / 6;
  const grid = new Uint16Array(res * res);
  let outside = 0;
  let zeroArea = 0;
  for (let t = 0; t < n; t++) {
    const a = [uv[t * 6], uv[t * 6 + 1]], b = [uv[t * 6 + 2], uv[t * 6 + 3]], c = [uv[t * 6 + 4], uv[t * 6 + 5]];
    for (const p of [a, b, c]) if (p[0] < -0.001 || p[0] > 1.001 || p[1] < -0.001 || p[1] > 1.001) outside++;
    const area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    if (Math.abs(area) < 1e-12) { zeroArea++; continue; }
    const x0 = Math.max(0, Math.floor(Math.min(a[0], b[0], c[0]) * res)), x1 = Math.min(res - 1, Math.ceil(Math.max(a[0], b[0], c[0]) * res));
    const y0 = Math.max(0, Math.floor(Math.min(a[1], b[1], c[1]) * res)), y1 = Math.min(res - 1, Math.ceil(Math.max(a[1], b[1], c[1]) * res));
    for (let y = y0; y <= y1; y++)
      for (let x = x0; x <= x1; x++) {
        // sample slightly off-center: grid-aligned UV edges would otherwise hit two triangles at once
        const px = (x + 0.5137) / res, py = (y + 0.4871) / res;
        const w0 = (b[0] - px) * (c[1] - py) - (c[0] - px) * (b[1] - py);
        const w1 = (c[0] - px) * (a[1] - py) - (a[0] - px) * (c[1] - py);
        const w2 = (a[0] - px) * (b[1] - py) - (b[0] - px) * (a[1] - py);
        if ((w0 > 0 && w1 > 0 && w2 > 0) || (w0 < 0 && w1 < 0 && w2 < 0)) {
          if (grid[y * res + x] < 65535) grid[y * res + x]++;
        }
      }
  }
  let covered = 0, multi = 0;
  for (const v of grid) {
    if (v) covered++;
    if (v > 1) multi++;
  }
  return {
    triangles: n,
    coverage: covered / (res * res),
    overlap: covered ? multi / covered : 0,
    outside: outside / Math.max(1, n * 3),
    degenerate: zeroArea / Math.max(1, n),
  };
}

// Unique UVs for any mesh: every triangle gets its own spot in a grid atlas.
// Consecutive triangles that share an edge (the two halves of a quad - the usual
// case) share one cell with a continuous diagonal; the rest get half a cell each.
// Cells come in three sizes by surface area. Painting is projected in 3D, so the
// many seams never show in the paint.
export function atlasUnwrap(geometry, texSize = 2048) {
  const pos = geometry.attributes.position.array;
  const n = pos.length / 9;
  const P = (t, k) => [pos[t * 9 + k * 3], pos[t * 9 + k * 3 + 1], pos[t * 9 + k * 3 + 2]];
  const same = (a, b) => Math.abs(a[0] - b[0]) + Math.abs(a[1] - b[1]) + Math.abs(a[2] - b[2]) < 1e-7;
  const area = (t) => {
    const a = P(t, 0), b = P(t, 1), c = P(t, 2);
    const u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]], v = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
    return 0.5 * Math.hypot(u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]);
  };
  // groups: [ta, tb, mapping] - quads (shared edge) or loose pairs
  const groups = [];
  let pending = -1;
  for (let t = 0; t < n; t++) {
    if (t + 1 < n) {
      // find shared vertices between t and t+1
      const sh = [];
      for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) if (same(P(t, i), P(t + 1, j))) sh.push([i, j]);
      if (sh.length === 2) {
        groups.push({ quad: true, a: t, b: t + 1, sh, w: area(t) + area(t + 1) });
        t++;
        continue;
      }
    }
    if (pending < 0) pending = t;
    else { groups.push({ quad: false, a: pending, b: t, w: area(pending) + area(t) }); pending = -1; }
  }
  if (pending >= 0) groups.push({ quad: false, a: pending, b: -1, w: area(pending) });
  let maxW = 0;
  for (const g of groups) maxW = Math.max(maxW, g.w);
  for (const g of groups) { const r = g.w / (maxW || 1); g.m = r > 0.25 ? 4 : r > 0.0625 ? 2 : 1; }
  groups.sort((x, y) => y.m - x.m);
  let units = 0;
  for (const g of groups) units += g.m * g.m;
  let grid = Math.ceil(Math.sqrt(units * 1.08)) + 4;
  const uv = new Float32Array(n * 6);
  const tryPack = (gsz) => {
    const cell = 1 / gsz;
    const pad = Math.min(cell * 0.12, 3 / Math.min(texSize, 1024));  // a few texels even at low resolutions
    let x = 0, y = 0, shelf = 0;
    for (const g of groups) {
      const m = g.m;
      if (x + m > gsz) { x = 0; y += shelf; shelf = 0; }
      if (y + m > gsz) return false;
      const x0 = x * cell + pad, y0 = y * cell + pad, s = m * cell - 2 * pad;
      if (g.quad) {
        // a's free corner at (x0,y0), shared edge along the diagonal, b's free corner opposite
        const freeA = [0, 1, 2].find((i) => !g.sh.some((e) => e[0] === i));
        const freeB = [0, 1, 2].find((j) => !g.sh.some((e) => e[1] === j));
        const c00 = [x0, y0], c10 = [x0 + s, y0], c01 = [x0, y0 + s], c11 = [x0 + s, y0 + s];
        uv.set(c00, g.a * 6 + freeA * 2);
        uv.set(c10, g.a * 6 + g.sh[0][0] * 2);
        uv.set(c01, g.a * 6 + g.sh[1][0] * 2);
        uv.set(c10, g.b * 6 + g.sh[0][1] * 2);
        uv.set(c01, g.b * 6 + g.sh[1][1] * 2);
        uv.set(c11, g.b * 6 + freeB * 2);
      } else {
        const q = s - pad;
        uv.set([x0, y0, x0 + q, y0, x0, y0 + q], g.a * 6);
        if (g.b >= 0) uv.set([x0 + s, y0 + s, x0 + pad + 0.0, y0 + s, x0 + s, y0 + pad], g.b * 6);
      }
      x += m;
      shelf = Math.max(shelf, m);
    }
    return true;
  };
  while (!tryPack(grid)) grid = Math.ceil(grid * 1.1) + 1;
  geometry.setAttribute('uv', new THREE.BufferAttribute(uv, 2));
  geometry.userData.atlas = true;
  return { triangles: n, grid };
}

// Tangents for normal mapping (non-indexed geometry). Without them three.js builds the
// tangent frame from screen-space derivatives, which breaks into lines along UV seams.
// Per-triangle tangents are averaged over vertices that share position and UV.
export function computeTangents(geometry) {
  const pos = geometry.attributes.position.array, nrm = geometry.attributes.normal.array, uv = geometry.attributes.uv.array;
  const n = pos.length / 9;
  const tan = new Float32Array(n * 3 * 4);
  const acc = new Map();
  const tri = [];
  for (let t = 0; t < n; t++) {
    const i0 = t * 3, i1 = i0 + 1, i2 = i0 + 2;
    const e1 = [pos[i1 * 3] - pos[i0 * 3], pos[i1 * 3 + 1] - pos[i0 * 3 + 1], pos[i1 * 3 + 2] - pos[i0 * 3 + 2]];
    const e2 = [pos[i2 * 3] - pos[i0 * 3], pos[i2 * 3 + 1] - pos[i0 * 3 + 1], pos[i2 * 3 + 2] - pos[i0 * 3 + 2]];
    const du1 = uv[i1 * 2] - uv[i0 * 2], dv1 = uv[i1 * 2 + 1] - uv[i0 * 2 + 1];
    const du2 = uv[i2 * 2] - uv[i0 * 2], dv2 = uv[i2 * 2 + 1] - uv[i0 * 2 + 1];
    const det = du1 * dv2 - du2 * dv1;
    const r = Math.abs(det) > 1e-20 ? 1 / det : 0;
    const T = [(e1[0] * dv2 - e2[0] * dv1) * r, (e1[1] * dv2 - e2[1] * dv1) * r, (e1[2] * dv2 - e2[2] * dv1) * r];
    const B = [(e2[0] * du1 - e1[0] * du2) * r, (e2[1] * du1 - e1[1] * du2) * r, (e2[2] * du1 - e1[2] * du2) * r];
    tri.push(T, B);
    for (const i of [i0, i1, i2]) {
      const key = `${pos[i * 3].toFixed(5)},${pos[i * 3 + 1].toFixed(5)},${pos[i * 3 + 2].toFixed(5)},${uv[i * 2].toFixed(5)},${uv[i * 2 + 1].toFixed(5)}`;
      let a = acc.get(key);
      if (!a) acc.set(key, (a = [0, 0, 0, 0, 0, 0]));
      a[0] += T[0]; a[1] += T[1]; a[2] += T[2]; a[3] += B[0]; a[4] += B[1]; a[5] += B[2];
    }
  }
  for (let i = 0; i < n * 3; i++) {
    const key = `${pos[i * 3].toFixed(5)},${pos[i * 3 + 1].toFixed(5)},${pos[i * 3 + 2].toFixed(5)},${uv[i * 2].toFixed(5)},${uv[i * 2 + 1].toFixed(5)}`;
    const a = acc.get(key);
    const N = [nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]];
    let T = [a[0], a[1], a[2]];
    const d = T[0] * N[0] + T[1] * N[1] + T[2] * N[2];
    T = [T[0] - N[0] * d, T[1] - N[1] * d, T[2] - N[2] * d];
    let len = Math.hypot(T[0], T[1], T[2]);
    if (len < 1e-12) { // degenerate: any vector perpendicular to N
      T = Math.abs(N[0]) < 0.9 ? [0, -N[2], N[1]] : [-N[2], 0, N[0]];
      len = Math.hypot(T[0], T[1], T[2]) || 1;
    }
    T = [T[0] / len, T[1] / len, T[2] / len];
    // handedness: does cross(N, T) point along the UV bitangent?
    const C = [N[1] * T[2] - N[2] * T[1], N[2] * T[0] - N[0] * T[2], N[0] * T[1] - N[1] * T[0]];
    const w = C[0] * a[3] + C[1] * a[4] + C[2] * a[5] < 0 ? -1 : 1;
    tan.set([T[0], T[1], T[2], w], i * 4);
  }
  geometry.setAttribute('tangent', new THREE.BufferAttribute(tan, 4));
}

// UV islands placed in other integer tiles (the Khronos DamagedHelmet sits in 1..2,
// UDIM-style layouts use several) are moved into 0..1 when the whole triangle lies
// inside one tile. Returns the number of triangles moved.
export function normalizeUVTiles(geometry) {
  const uv = geometry.attributes.uv.array;
  let moved = 0;
  for (let t = 0; t < uv.length / 6; t++) {
    const o = t * 6;
    const fu = Math.floor((uv[o] + uv[o + 2] + uv[o + 4]) / 3), fv = Math.floor((uv[o + 1] + uv[o + 3] + uv[o + 5]) / 3);
    if (fu === 0 && fv === 0) continue;
    let inside = true;
    for (let k = 0; k < 3; k++) {
      const u = uv[o + k * 2] - fu, v = uv[o + k * 2 + 1] - fv;
      if (u < -1e-4 || u > 1 + 1e-4 || v < -1e-4 || v > 1 + 1e-4) inside = false;
    }
    if (!inside) continue;
    for (let k = 0; k < 3; k++) { uv[o + k * 2] -= fu; uv[o + k * 2 + 1] -= fv; }
    moved++;
  }
  if (moved) geometry.attributes.uv.needsUpdate = true;
  return moved;
}
