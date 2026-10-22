// Patina Web: tests of the island pipeline (islands.js) in Node, without a browser.
//   node tests/web/test_islands.mjs
import * as Isl from '../../web/src/islands.js';

let failed = 0;
const check = (ok, what) => { if (!ok) { failed++; console.log('  FAIL', what); } };

// a curved grid symmetric in X as non-indexed triangles; `split`: which quads become two triangles
// (always along the same world diagonal, so the halves are triangulated differently); `warp`: the
// +X half is bent out of shape (more than half an edge)
function grid(N, warp) {
  const P = [];
  const pt = (i, j) => {
    let x = -1 + (2 * i) / N, z = -1 + (2 * j) / N, y = 0.25 * Math.cos(2 * x);
    if (warp && x > 0) { const k = Math.min(1, 4 * x); x += 0.12 * Math.sin(2.5 * z) * k; y += 0.1 * Math.sin(3 * z + 1) * k; }
    return [x, y, z];
  };
  for (let j = 0; j < N; j++)
    for (let i = 0; i < N; i++) {
      const a = pt(i, j), b = pt(i + 1, j), c = pt(i + 1, j + 1), d = pt(i, j + 1);
      if ((i + j) % 3 === 0) P.push(...a, ...b, ...c, ...a, ...c, ...d);
      else P.push(...a, ...b, ...d, ...b, ...c, ...d);  // the other diagonal
    }
  return { attributes: { position: { array: new Float32Array(P), count: P.length / 3 } } };
}

for (const warp of [false, true]) {
  const name = warp ? 'warped' : 'symmetric';
  const g = grid(12, warp);
  const topo = new Isl.Topo(g);
  const T = topo.T;
  const uv = new Float32Array(6 * T), flags = new Uint8Array(T);
  // a patch on the +X side, mirrored
  const sel = new Uint8Array(T);
  for (let t = 0; t < T; t++) { const c = topo.c(t); if (c[0] > 0.3 && c[0] < 0.85 && c[2] > -0.6 && c[2] < 0.5) sel[t] = 1; }
  const before = sel.reduce((s, v) => s + v, 0);
  Isl.mirrorSelection(topo, sel, 1);
  let neg = 0, pos = 0;
  for (let t = 0; t < T; t++) if (sel[t]) (topo.center[3 * t] < 0 ? neg++ : pos++);
  check(neg >= before - 2 && neg <= pos + 4, `${name}: mirrored selection ${neg} for ${pos} (patch ${before})`);
  const tris = [];
  for (let t = 0; t < T; t++) if (sel[t]) tris.push(t);
  const r = Isl.makeIsland(topo, uv, flags, tris, { orient: 'upright', density: 0.3, axes: 1, pin: true });
  check(r.ok, `${name}: island made (${r.message})`);
  check(r.stacked === neg, `${name}: every mirrored triangle stacked (${r.stacked} of ${neg})`);
  const links = Isl.mirrorLinks(topo, flags.slice(), [uv]);
  check(links.copies === neg, `${name}: links ${links.copies}`);
  check(Isl.unstackedCount(uv, links) === 0, `${name}: stacked exactly`);
  let flipped = 0;
  for (const t of tris) if (!(flags[t] & Isl.F_COPY) && Isl.triUVArea(uv, t) <= 0) flipped++;
  check(flipped === 0, `${name}: no flipped triangles (${flipped})`);
  check(tris.every((t) => flags[t] & Isl.F_PIN), `${name}: pinned`);
  // the island lies inside 0..1 and the stacked half is one island on the other
  let ok = true;
  for (const t of tris) for (let k = 0; k < 6; k++) if (uv[6 * t + k] < -1e-4 || uv[6 * t + k] > 1.0001) ok = false;
  check(ok, `${name}: inside 0..1`);
  const isl = Isl.uvIslands(topo, uv);
  const ids = new Set(tris.map((t) => isl.id[t]));
  check(ids.size === 2, `${name}: two islands (half and stacked half), got ${ids.size}`);
  // packing around the pinned island leaves it alone
  const keep = uv.slice();
  const k = Isl.repackMovable(topo, uv, flags, 0.3);
  check(k > 0, `${name}: repacked the rest`);
  check(tris.every((t) => [0, 1, 2, 3, 4, 5].every((q) => uv[6 * t + q] === keep[6 * t + q])), `${name}: pinned island unchanged`);
}
// ---- (0.43) symmetric but not on the plane: the middle wanders (a fifth of an edge), the lower rows are shifted
// sideways, and the cloth has thickness (a layer under it, joined round the rim by short edges). Up to 0.42.0 the
// triangles along the middle were never stacked.
{
  const N = 16, e = 2 / N, th = e / 12;
  const pt = (layer, i, j) => {
    let x = -1 + (2 * i) / N, z = -1 + (2 * j) / N, y = 0.25 * Math.cos(2 * x) - layer * th;
    if (i === N / 2) x += 0.2 * e * Math.sin(3 * z + 0.5);
    if (j > N / 2) x += 0.15 * e;
    return [x, y, z];
  };
  const P = [];
  const quad = (a, b, c, d) => P.push(...a, ...b, ...c, ...a, ...c, ...d);
  for (let j = 0; j < N; j++)
    for (let i = 0; i < N; i++) {
      quad(pt(0, i, j), pt(0, i, j + 1), pt(0, i + 1, j + 1), pt(0, i + 1, j));
      quad(pt(1, i, j), pt(1, i + 1, j), pt(1, i + 1, j + 1), pt(1, i, j + 1));
    }
  for (let k = 0; k < N; k++) {
    quad(pt(0, k, 0), pt(0, k + 1, 0), pt(1, k + 1, 0), pt(1, k, 0));
    quad(pt(0, k + 1, N), pt(0, k, N), pt(1, k, N), pt(1, k + 1, N));
    quad(pt(0, 0, k + 1), pt(0, 0, k), pt(1, 0, k), pt(1, 0, k + 1));
    quad(pt(0, N, k), pt(0, N, k + 1), pt(1, N, k + 1), pt(1, N, k));
  }
  const topo = new Isl.Topo({ attributes: { position: { array: new Float32Array(P), count: P.length / 3 } } });
  const T = topo.T, uv = new Float32Array(6 * T), flags = new Uint8Array(T);
  const tris = [];
  let neg = 0;
  for (let t = 0; t < T; t++) { tris.push(t); if (topo.center[3 * t] < -topo.eps) neg++; }
  const r = Isl.makeIsland(topo, uv, flags, tris, { orient: 'upright', density: 0.3, axes: 1 });
  check(r.ok, `wandering middle: island made (${r.message})`);
  check(r.stacked === neg, `wandering middle: every triangle on the -X side stacked (${r.stacked} of ${neg})`);
}

// ---- relax: a sheared grid island becomes conformal again, same place and size
{
  const g = grid(8, false);
  const topo = new Isl.Topo(g);
  const T = topo.T, uv = new Float32Array(6 * T), flags = new Uint8Array(T);
  for (let t = 0; t < T; t++) for (let k = 0; k < 3; k++) {
    const p = topo.p(topo.canon[3 * t + k]);
    uv[6 * t + 2 * k] = 0.2 + (p[0] + 1) * 0.15 + (p[2] + 1) * 0.08;
    uv[6 * t + 2 * k + 1] = 0.3 + (p[2] + 1) * 0.06;
  }
  const angleErr = () => {  // mean |angle in 3D - angle in UV| over the corners
    let e = 0;
    for (let t = 0; t < T; t++) for (let k = 0; k < 3; k++) {
      const a = topo.p(topo.canon[3 * t + k]), b = topo.p(topo.canon[3 * t + (k + 1) % 3]), c = topo.p(topo.canon[3 * t + (k + 2) % 3]);
      const ang3 = (u, v) => Math.acos(Math.max(-1, Math.min(1, (u[0] * v[0] + u[1] * v[1] + u[2] * v[2]) / (Math.hypot(...u) * Math.hypot(...v)))));
      const A = [6 * t + 2 * k, 6 * t + 2 * ((k + 1) % 3), 6 * t + 2 * ((k + 2) % 3)].map((o) => [uv[o], uv[o + 1]]);
      const ang2 = (u, v) => Math.acos(Math.max(-1, Math.min(1, (u[0] * v[0] + u[1] * v[1]) / (Math.hypot(...u) * Math.hypot(...v)))));
      e += Math.abs(ang3([b[0] - a[0], b[1] - a[1], b[2] - a[2]], [c[0] - a[0], c[1] - a[1], c[2] - a[2]]) -
                    ang2([A[1][0] - A[0][0], A[1][1] - A[0][1]], [A[2][0] - A[0][0], A[2][1] - A[0][1]]));
    }
    return e / (3 * T);
  };
  const area = () => { let a = 0; for (let t = 0; t < T; t++) a += Isl.triUVArea(uv, t); return a; };
  const center = () => { let x = 0, y = 0; for (let i = 0; i < uv.length; i += 2) { x += uv[i]; y += uv[i + 1]; } return [x / (uv.length / 2), y / (uv.length / 2)]; };
  const e0 = angleErr(), a0 = area(), c0 = center();
  const n = Isl.relaxIslands(topo, uv, flags, [0]);
  const e1 = angleErr(), a1 = area(), c1 = center();
  check(n === 1, 'relax: one island');
  check(e1 < e0 * 0.05, `relax: stretch gone (angle error ${e0.toFixed(3)} -> ${e1.toFixed(4)})`);
  check(Math.abs(a1 - a0) < 1e-4 * Math.abs(a0), 'relax: same area');
  check(Math.hypot(c1[0] - c0[0], c1[1] - c0[1]) < 0.02, 'relax: same place');
}

// ---- packing by shape: inside 0..1, no overlaps, pinned islands untouched, tighter than boxes
{
  const g = grid(12, false);
  const topo = new Isl.Topo(g);
  const T = topo.T;
  // every quad (2 triangles) its own island, in random places, scaled differently
  const uv = new Float32Array(6 * T), flags = new Uint8Array(T);
  let seed = 3;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648);
  const charts = [];
  for (let t = 0; t < T; t += 2) {
    const s = 0.05 + rnd() * 0.05, ox = rnd() * 0.8, oy = rnd() * 0.8;
    for (const tt of [t, t + 1]) for (let k = 0; k < 3; k++) {
      const p = topo.p(topo.canon[3 * tt + k]);
      uv[6 * tt + 2 * k] = ox + (p[0] + 1) * s * 6; uv[6 * tt + 2 * k + 1] = oy + (p[2] + 1) * s * 6;
    }
    charts.push([t, t + 1]);
  }
  // pin the first one where it is
  flags[0] = flags[1] = Isl.F_PIN;
  const pinnedUV = uv.slice(0, 12);
  const k = Isl.packShapes(topo, uv, charts.slice(1), { obstacle: (t) => flags[t] & Isl.F_PIN });
  check(k > 0, 'pack: packed');
  let inside = true;
  for (let i = 0; i < uv.length; i++) if (uv[i] < -1e-6 || uv[i] > 1 + 1e-6) inside = false;
  check(inside, 'pack: inside 0..1');
  check(pinnedUV.every((v, i) => v === uv[i]), 'pack: the pinned island stays');
  // overlaps: cell centers covered by two different islands
  const R = 256, owner = new Int32Array(R * R).fill(-1);
  let clash = 0;
  charts.forEach((c, id) => {
    const seen = new Set();
    for (const t of c) {
      const o = 6 * t, a = [uv[o], uv[o + 1]], b = [uv[o + 2], uv[o + 3]], d = [uv[o + 4], uv[o + 5]];
      const x0 = Math.floor(Math.min(a[0], b[0], d[0]) * R), x1 = Math.ceil(Math.max(a[0], b[0], d[0]) * R);
      const y0 = Math.floor(Math.min(a[1], b[1], d[1]) * R), y1 = Math.ceil(Math.max(a[1], b[1], d[1]) * R);
      const ed = (p, q, r) => (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0]);
      const sgn = Math.sign(ed(a, b, d));
      for (let y = Math.max(0, y0); y < Math.min(R, y1); y++) for (let x = Math.max(0, x0); x < Math.min(R, x1); x++) {
        const p = [(x + 0.5) / R, (y + 0.5) / R];
        if (sgn * ed(a, b, p) >= 0 && sgn * ed(b, d, p) >= 0 && sgn * ed(d, a, p) >= 0 && !seen.has(y * R + x)) {
          seen.add(y * R + x);
          if (owner[y * R + x] >= 0 && owner[y * R + x] !== id) clash++;
          owner[y * R + x] = id;
        }
      }
    }
  });
  check(clash === 0, `pack: no overlaps (${clash} cells)`);
  let used = 0;
  for (const o of owner) if (o >= 0) used++;
  check(used / (R * R) > 0.5, `pack: ${(100 * used / (R * R)).toFixed(0)}% of the texture used`);
}

// texel density targets, against layouts whose answer is known: square quads in the XZ plane,
// each its own island, side `s` in the model and `us` in UV (density us / s)
{
  const P = [], UV = [];
  const quad = (x, z, s, u, v, us) => {
    const p = [[x, 0, z], [x + s, 0, z], [x + s, 0, z - s], [x, 0, z - s]], q = [[u, v], [u + us, v], [u + us, v + us], [u, v + us]];
    for (const k of [0, 1, 2, 0, 2, 3]) { P.push(...p[k]); UV.push(...q[k]); }
  };
  const build = () => {
    const topo = new Isl.Topo({ attributes: { position: { array: new Float32Array(P), count: P.length / 3 } } });
    return { topo, uv: new Float32Array(UV), flags: new Uint8Array(topo.T) };
  };
  // one big island (area 4, density 0.25) and 40 small ones (total area 0.4, density 0.5):
  // the layout is the big one's, by area (a plain median of the islands would say 0.5)
  quad(0, 0, 2, 0.02, 0.02, 0.5);
  for (let i = 0; i < 40; i++) quad(3 + (i % 8) * 0.2, -(i >> 3) * 0.2, 0.1, 0.56 + (i % 8) * 0.055, 0.02 + (i >> 3) * 0.055, 0.05);
  let { topo, uv, flags } = build();
  let d = Isl.densityTargets(topo, uv, flags, null);
  check(Math.abs(d.layout - 0.25) < 1e-3, `density: layout by area ${d.layout.toFixed(4)} (0.25)`);
  check(Math.abs(d.fit - Math.sqrt(0.7 / 4.4)) < 1e-4, `density: fit ${d.fit.toFixed(4)}`);
  // mirror copies share texels: they don't count toward fit (the 40 small quads as copies)
  for (let t = 2; t < topo.T; t++) flags[t] |= Isl.F_COPY;
  d = Isl.densityTargets(topo, uv, flags, null);
  check(Math.abs(d.fit - Math.sqrt(0.7 / 4)) < 1e-4, `density: fit without mirror copies ${d.fit.toFixed(4)}`);
  // tiled UVs (the big island spans 1..5) are not part of the layout
  P.length = 0; UV.length = 0;
  quad(0, 0, 2, 1, 1, 4);
  quad(3, 0, 1, 0.1, 0.1, 0.25);
  ({ topo, uv, flags } = build());
  d = Isl.densityTargets(topo, uv, flags, null);
  check(Math.abs(d.layout - 0.25) < 1e-3, `density: tiled island ignored, layout ${d.layout.toFixed(4)} (0.25)`);
  // a fully stacked layout (10 unit quads on the same 0..1 square) is too dense to grow into
  P.length = 0; UV.length = 0;
  for (let i = 0; i < 10; i++) quad(i * 1.5, 0, 1, 0, 0, 1);
  ({ topo, uv, flags } = build());
  d = Isl.densityTargets(topo, uv, flags, null);
  check(Math.abs(d.layout - 1) < 1e-3 && Math.abs(d.auto - 2 * Math.sqrt(0.07)) < 1e-3,
    `density: stacked layout ${d.layout.toFixed(3)} capped to ${d.auto.toFixed(3)}`);
  // the Island brush gives a new island the layout density
  P.length = 0; UV.length = 0;
  quad(0, 0, 2, 0.02, 0.02, 0.5);
  for (let i = 0; i < 4; i++) quad(3 + i * 0.6, 0, 0.5, 0.6 + (i % 2) * 0.17, 0.05 + (i >> 1) * 0.17, 0.125);
  ({ topo, uv, flags } = build());
  const sel = [4, 5, 6, 7, 8, 9];   // three small quads
  const ex = new Uint8Array(topo.T);
  for (const t of sel) ex[t] = 1;
  const target = Isl.densityTargets(topo, uv, flags, ex).auto;
  const r = Isl.makeIsland(topo, uv, flags, sel, { orient: 'none', density: target, axes: 0, pin: false });
  check(r.ok && !r.repacked, `density: island made without a repack (${r.message})`);
  check(Math.abs(Isl.density(topo, uv, sel) / 0.25 - 1) < 0.02, `density: new island ${Isl.density(topo, uv, sel).toFixed(4)} (0.25)`);
}

console.log(failed ? `${failed} failed` : 'islands.js: all checks passed');
process.exit(failed ? 1 : 0);
