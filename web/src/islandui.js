// Patina Web - the Island brush and UV island editing (UI side). The pipeline is in islands.js.
import * as THREE from 'three';
import * as Isl from './islands.js';

// ctx: { renderer, camera, canvas, mesh, engine, state, status, $, uvCanvas, cssSize, afterLayout,
//        updateUndoButtons, requestUV }
export function setupIslands(ctx) {
  const { renderer, camera, mesh, engine, state, $ } = ctx;
  const I = (state.island = {
    orient: 'upright', density: 'auto', custom: 1024, axes: 1, pin: true, keepPaint: true,
    sel: new Uint8Array(0), flags: new Uint8Array(0), version: 0,
    topo: null, cache: null, uvMode: 'paint', drag: null, brushing: false,
  });

  // ---------------------------------------------------------------- geometry
  engine.getMirrorFlags = () => I.flags;
  function onGeometry(g) {
    const T = g.attributes.position.count / 3;
    I.sel = new Uint8Array(T);
    I.flags = new Uint8Array(T);
    I.topo = null;
    I.cache = null;
    I.version++;
    const tri = new Float32Array(3 * T);
    for (let t = 0; t < T; t++) tri[3 * t] = tri[3 * t + 1] = tri[3 * t + 2] = t;
    g.setAttribute('aTri', new THREE.BufferAttribute(tri, 1));
    g.setAttribute('aSel', new THREE.BufferAttribute(new Float32Array(3 * T), 1));
  }
  const topo = () => (I.topo ||= new Isl.Topo(mesh.geometry));
  const uvArr = () => mesh.geometry.attributes.uv.array;

  function selectedTris() {
    const out = [];
    for (let t = 0; t < I.sel.length; t++) if (I.sel[t]) out.push(t);
    return out;
  }
  // the 3D view's aSel: 1 selected, + 2 pinned (striped blue while the Island brush is chosen)
  function syncSel(tris) {
    const a = mesh.geometry.attributes.aSel;
    if (!a) return;
    const v = (t) => I.sel[t] + (I.flags[t] & Isl.F_PIN ? 2 : 0);
    if (tris) for (const t of tris) a.array[3 * t] = a.array[3 * t + 1] = a.array[3 * t + 2] = v(t);
    else for (let t = 0; t < I.sel.length; t++) a.array[3 * t] = a.array[3 * t + 1] = a.array[3 * t + 2] = v(t);
    a.needsUpdate = true;
    ctx.requestUV();
  }
  function setSelection(tris, mode = 'set') {
    if (mode === 'set') I.sel.fill(0);
    for (const t of tris) I.sel[t] = mode === 'remove' ? 0 : 1;
    syncSel();
    updatePanel();
  }

  // islands, links and markers of the current layout (cached per edit)
  function info() {
    const key = mesh.geometry.attributes.uv.version + ':' + I.version;
    if (I.cache && I.cache.key === key) return I.cache;
    const tp = topo(), uv = uvArr();
    const isl = Isl.uvIslands(tp, uv);
    const lists = Isl.islandLists(isl);
    const flags = I.flags.slice();
    const links = Isl.mirrorLinks(tp, flags, [uv]);
    const per = lists.map((l) => {
      let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity, pinned = false, copies = 0;
      for (const t of l) {
        for (let k = 0; k < 3; k++) { const c = 3 * t + k; x0 = Math.min(x0, uv[2 * c]); x1 = Math.max(x1, uv[2 * c]); y0 = Math.min(y0, uv[2 * c + 1]); y1 = Math.max(y1, uv[2 * c + 1]); }
        if (I.flags[t] & Isl.F_PIN) pinned = true;
        if (I.flags[t] & Isl.F_COPY) copies++;
      }
      return { tris: l, x0, y0, x1, y1, pinned, isCopy: copies === l.length, stacked: 0 };
    });
    for (let t = 0; t < tp.T; t++) if (links.src[3 * t] >= 0) per[isl.id[(links.src[3 * t] / 3) | 0]].stacked++;
    I.cache = { key, isl, lists, per, links, unstacked: Isl.unstackedCount(uv, links) };
    return I.cache;
  }

  // ---------------------------------------------------------------- applying a UV edit
  // pins keep their UVs, mirror copies are restacked on their other half, paint follows the new
  // layout (when "Keep paint" is on), and it is one undo step
  function applyUVEdit(newUV, newFlags, label) {
    const g = mesh.geometry, attr = g.attributes.uv;
    const oldUV = attr.array.slice(), oldFlags = I.flags.slice();
    const T = I.flags.length;
    for (let t = 0; t < T; t++)
      if (oldFlags[t] & Isl.F_PIN && newFlags[t] & Isl.F_PIN) for (let k = 0; k < 6; k++) newUV[6 * t + k] = oldUV[6 * t + k];
    const links = Isl.mirrorLinks(topo(), newFlags, [newUV, oldUV]);
    Isl.restack(newUV, links);
    let same = true;
    for (let i = 0; i < newUV.length && same; i++) if (newUV[i] !== oldUV[i]) same = false;
    for (let t = 0; t < T && same; t++) if (newFlags[t] !== oldFlags[t]) same = false;
    if (same) return false;
    attr.array.set(newUV);
    attr.needsUpdate = true;
    const targets = I.keepPaint ? reproject(oldUV, newUV, newFlags) : [];
    I.flags = newFlags;
    I.version++;
    syncSel();
    updatePanel();
    ctx.afterLayout();
    engine.pushUndo({ kind: 'uv', uv: oldUV, flags: oldFlags, targets, label });
    ctx.updateUndoButtons();
    return true;
  }
  engine.onUVSwap = (e) => {
    const attr = mesh.geometry.attributes.uv;
    const cur = attr.array.slice();
    attr.array.set(e.uv);
    e.uv = cur;
    attr.needsUpdate = true;
    const f = I.flags;
    I.flags = e.flags;
    e.flags = f;
    I.version++;
    syncSel();
    updatePanel();
    ctx.afterLayout();
  };
  function flagsOnly(newFlags, label) {
    applyUVEdit(uvArr().slice(), newFlags, label);
  }

  // paint moves with the layout: rasterize the new layout, sample the old textures at the old UVs
  function reproject(oldUV, newUV, flags) {
    // only the triangles that own their texels: mirror copies share the texels of the half they
    // copy, and drawing both would interleave their old paint triangle by triangle
    const keep = [];
    for (let t = 0; t < flags.length; t++) if (!(flags[t] & Isl.F_COPY)) keep.push(t);
    const g = new THREE.BufferGeometry();
    const pos = new Float32Array(keep.length * 9), nu = new Float32Array(keep.length * 6), ou = new Float32Array(keep.length * 6);
    const P = mesh.geometry.attributes.position.array;
    keep.forEach((t, i) => {
      pos.set(P.subarray(9 * t, 9 * t + 9), 9 * i);
      nu.set(newUV.subarray(6 * t, 6 * t + 6), 6 * i);
      ou.set(oldUV.subarray(6 * t, 6 * t + 6), 6 * i);
    });
    g.setAttribute('position', new THREE.BufferAttribute(pos, 3));
    g.setAttribute('uv', new THREE.BufferAttribute(nu, 2));
    g.setAttribute('uvOld', new THREE.BufferAttribute(ou, 2));
    const mat = new THREE.RawShaderMaterial({
      glslVersion: THREE.GLSL3,
      vertexShader: `precision highp float; in vec3 position; in vec2 uv; in vec2 uvOld; out vec2 vOld;
        void main(){ vOld = uvOld; gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0); }`,
      fragmentShader: `precision highp float; in vec2 vOld; uniform sampler2D uSrc; out vec4 o; void main(){ o = texture(uSrc, vOld); }`,
      uniforms: { uSrc: { value: null } }, side: THREE.DoubleSide, depthTest: false, depthWrite: false,
    });
    const sc = new THREE.Scene();
    const m = new THREE.Mesh(g, mat);
    m.frustumCulled = false;
    sc.add(m);
    const prev = renderer.getRenderTarget();
    const saved = [];
    for (const l of engine.layers)
      for (const k of ['color', 'rmh', 'mask']) {
        if (!l[k]) continue;
        const dst = new THREE.WebGLRenderTarget(engine.size, engine.size, { depthBuffer: false });
        engine._clear(dst, k === 'rmh' ? [0.5, 0, 0.5, 0] : k === 'mask' ? [0, 0, 0, 1] : [0, 0, 0, 0]);
        mat.uniforms.uSrc.value = l[k].texture;
        renderer.setRenderTarget(dst);
        const ac = renderer.autoClear;
        renderer.autoClear = false;
        renderer.render(sc, engine.quadCam);
        renderer.autoClear = ac;
        saved.push({ layer: l, key: k, rt: l[k] });
        l[k] = dst;
      }
    renderer.setRenderTarget(prev);
    mat.dispose();
    g.dispose();
    return saved;
  }

  // ---------------------------------------------------------------- the Island brush (3D view)
  const idMat = new THREE.RawShaderMaterial({
    glslVersion: THREE.GLSL3,
    vertexShader: `precision highp float; in vec3 position; in float aTri; uniform mat4 modelViewMatrix; uniform mat4 projectionMatrix;
      flat out float vId; void main(){ vId = aTri; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`,
    fragmentShader: `precision highp float; flat in float vId; out vec4 o;
      void main(){ float id = vId + 1.0; o = vec4(mod(floor(id / 65536.0), 256.0), mod(floor(id / 256.0), 256.0), mod(id, 256.0), 255.0) / 255.0; }`,
  });
  const idScene = new THREE.Scene();
  const idMesh = new THREE.Mesh(undefined, idMat);
  idMesh.frustumCulled = false;
  idScene.add(idMesh);
  let idRT = null;
  function renderIds() {
    const v = ctx.cssSize();
    const w = Math.max(16, Math.round(v.w)), h = Math.max(16, Math.round(v.h));
    if (!idRT || idRT.width !== w || idRT.height !== h) {
      if (idRT) idRT.dispose();
      idRT = new THREE.WebGLRenderTarget(w, h, { depthBuffer: true, minFilter: THREE.NearestFilter, magFilter: THREE.NearestFilter });
    }
    idMesh.geometry = mesh.geometry;
    const prev = renderer.getRenderTarget(), cc = renderer.getClearColor(new THREE.Color()), ca = renderer.getClearAlpha();
    renderer.setRenderTarget(idRT);
    renderer.setClearColor(0x000000, 0);
    renderer.clear(true, true, false);
    renderer.render(idScene, camera);
    renderer.setRenderTarget(prev);
    renderer.setClearColor(cc, ca);
  }
  function brushAt(pt, remove) {
    if (!idRT) return;
    const r = Math.max(1, state.brush.size / 2);
    const x0 = Math.max(0, Math.floor(pt.x - r)), x1 = Math.min(idRT.width - 1, Math.ceil(pt.x + r));
    const y0 = Math.max(0, Math.floor(pt.y - r)), y1 = Math.min(idRT.height - 1, Math.ceil(pt.y + r));
    if (x1 < x0 || y1 < y0) return;
    const w = x1 - x0 + 1, h = y1 - y0 + 1;
    const px = new Uint8Array(w * h * 4);
    renderer.readRenderTargetPixels(idRT, x0, idRT.height - 1 - y1, w, h, px);
    const hit = new Set();
    for (let j = 0; j < h; j++)
      for (let i = 0; i < w; i++) {
        const sx = x0 + i, sy = y1 - j;  // row 0 of the read is the bottom
        if ((sx - pt.x) ** 2 + (sy - pt.y) ** 2 > r * r) continue;
        const o = (j * w + i) * 4;
        const id = px[o] * 65536 + px[o + 1] * 256 + px[o + 2];
        if (id) hit.add(id - 1);
      }
    const want = remove ? 0 : 1;
    const changed = [];
    for (const t of hit) if (t < I.sel.length && I.sel[t] !== want) { I.sel[t] = want; changed.push(t); }
    if (I.axes && changed.length) {
      const tp = topo();
      const extra = [];
      for (const t of changed)
        for (let sub = 1; sub < 8; sub++) {
          if ((sub & I.axes) !== sub) continue;
          for (const g of Isl.mirrorTris(tp, t, sub)) if (I.sel[g] !== want) { I.sel[g] = want; extra.push(g); }
        }
      changed.push(...extra);
    }
    if (changed.length) { syncSel(changed); updatePanel(); }
  }
  // the triangle under a viewport point (CSS px), or -1
  function pickTri(pt) {
    renderIds();
    const x = Math.round(pt.x), y = Math.round(pt.y);
    if (x < 0 || y < 0 || x >= idRT.width || y >= idRT.height) return -1;
    const px = new Uint8Array(4);
    renderer.readRenderTargetPixels(idRT, x, idRT.height - 1 - y, 1, 1, px);
    const id = px[0] * 65536 + px[1] * 256 + px[2];
    return id ? id - 1 : -1;
  }
  function brushDown(pt, e) {
    renderIds();
    I.brushing = true;
    brushAt(pt, e.ctrlKey || e.metaKey);
  }
  function brushMove(pt, e) { if (I.brushing) brushAt(pt, e.ctrlKey || e.metaKey); }
  function brushUp() { I.brushing = false; }

  function densityTarget(tp, uv, exclude) {
    const d = Isl.densityTargets(tp, uv, I.flags, exclude);
    if (I.density === 'layout') return d.layout || d.fit;
    if (I.density === 'fit') return d.fit;
    if (I.density === 'custom') {
      // texels per unit of the original model: the view scales models to a radius of 1
      const s = new THREE.Vector3().setFromMatrixScale(ctx.state.model.restore).x || 1;
      return (I.custom / engine.size) * s;
    }
    return d.auto;
  }

  function makeIsland() {
    const tris = selectedTris();
    if (!tris.length) { ctx.status('Paint over the model with the Island brush (L) first', true); return; }
    const tp = topo();
    const uv = uvArr().slice(), flags = I.flags.slice();
    const exclude = new Uint8Array(tp.T);
    for (const t of tris) exclude[t] = 1;
    const t0 = performance.now();
    const r = Isl.makeIsland(tp, uv, flags, tris, {
      orient: I.orient, density: densityTarget(tp, uv, exclude), axes: I.axes, pin: I.pin,
    });
    if (!r.ok) { ctx.status(r.message, true); return; }
    applyUVEdit(uv, flags, 'Make island');
    // what the new island actually got (less than the target when everything had to be repacked)
    const made = tris.filter((t) => !(I.flags[t] & Isl.F_COPY));
    const tpu = (Isl.density(tp, uvArr(), made) * engine.size) / (new THREE.Vector3().setFromMatrixScale(ctx.state.model.restore).x || 1);
    ctx.status(`Island made (${r.charts} piece${r.charts === 1 ? '' : 's'})` +
      (r.stacked ? `, mirrored half stacked on it (${r.stacked} triangles)` : '') +
      (I.pin ? ', pinned' : '') + (r.pinnedLeft ? `; ${r.pinnedLeft} pinned triangles left alone` : '') +
      (r.repacked ? ' - no free space, so the unpinned islands were repacked' : '') +
      ` · ${Math.round(tpu)} texels per unit · ${Math.round(performance.now() - t0)} ms`);
  }

  // ---------------------------------------------------------------- operations on selected islands
  // the islands of the selection; `layout`: what a layout operation acts on (a mirror copy stands
  // for the half it copies, pinned islands are left out)
  function selIslands(layout) {
    const c = info(), want = new Set();
    for (let t = 0; t < I.sel.length; t++) {
      if (!I.sel[t]) continue;
      let u = t;
      if (layout && c.links.src[3 * t] >= 0) u = (c.links.src[3 * t] / 3) | 0;
      want.add(c.isl.id[u]);
    }
    const out = [];
    let pinned = 0;
    for (const i of want) {
      const p = c.per[i];
      if (layout && p.pinned) { pinned++; continue; }
      if (layout && p.isCopy) continue;
      out.push(p.tris);
    }
    return { charts: out, pinned };
  }
  function noneMsg(pinned) {
    ctx.status(pinned ? 'Those islands are pinned: Alt+P (or right-click > Unpin) first' : 'Select islands first (UV view, Islands mode)', true);
  }
  function overlapsOthers(uv, chart) {
    const T = I.flags.length, inC = new Uint8Array(T);
    for (const t of chart) inC[t] = 1;
    const G = 256, occ = new Uint8Array(G * G);
    const mark = (t, grid, v) => {
      const o = 6 * t;
      const xs = [uv[o], uv[o + 2], uv[o + 4]].map((x) => x * G), ys = [uv[o + 1], uv[o + 3], uv[o + 5]].map((y) => y * G);
      const cx = Math.floor((xs[0] + xs[1] + xs[2]) / 3), cy = Math.floor((ys[0] + ys[1] + ys[2]) / 3);
      if (cx >= 0 && cx < G && cy >= 0 && cy < G) { if (v) grid[cy * G + cx] = 1; else return grid[cy * G + cx]; }
      return 0;
    };
    for (let t = 0; t < T; t++) if (!inC[t] && !(I.flags[t] & Isl.F_COPY)) mark(t, occ, 1);
    let hits = 0;
    for (const t of chart) hits += mark(t, occ, 0);
    return hits >= 2;
  }
  function islandOp(label, fn, resolve = true) {
    const { charts, pinned } = selIslands(true);
    if (!charts.length) return noneMsg(pinned);
    const tp = topo(), uv = uvArr().slice(), flags = I.flags.slice();
    for (const c of charts) fn(tp, uv, c);
    if (resolve) {
      const moved = charts.filter((c) => overlapsOthers(uv, c));
      if (moved.length) Isl.placeInFreeSpace(uv, flags, tp.T, moved, 0.005, true);
    }
    applyUVEdit(uv, flags, label);
    if (pinned) ctx.status(`${label}: ${pinned} pinned island${pinned === 1 ? '' : 's'} left alone`);
    else ctx.status(label);
  }
  // rigid transforms of the selection (both halves of stacked pairs move together)
  function transformSel(label, fn) {
    const c = info();
    const sel = selectedTris();
    if (!sel.length) return noneMsg(0);
    const tris = Isl.withLinked(topo(), c.links, sel).filter((t) => !(I.flags[t] & Isl.F_PIN));
    if (!tris.length) return noneMsg(1);
    const uv = uvArr().slice();
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (const t of sel) for (let k = 0; k < 3; k++) { const q = 3 * t + k; x0 = Math.min(x0, uv[2 * q]); x1 = Math.max(x1, uv[2 * q]); y0 = Math.min(y0, uv[2 * q + 1]); y1 = Math.max(y1, uv[2 * q + 1]); }
    const cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    for (const t of tris)
      for (let k = 0; k < 3; k++) {
        const q = 3 * t + k;
        const [u, v] = fn(uv[2 * q] - cx, uv[2 * q + 1] - cy);
        uv[2 * q] = u + cx; uv[2 * q + 1] = v + cy;
      }
    applyUVEdit(uv, I.flags.slice(), label);
  }
  function pinSel(pin) {
    const c = info();
    const sel = selectedTris();
    if (!sel.length) { ctx.status(pin ? 'Select the islands to pin first' : 'Select the islands to unpin first', true); return; }
    const want = new Set(sel.map((t) => c.isl.id[t]));
    let tris = [];
    for (const i of want) tris.push(...c.per[i].tris);
    tris = Isl.withLinked(topo(), c.links, tris);
    const flags = I.flags.slice();
    for (const t of tris) flags[t] = pin ? flags[t] | Isl.F_PIN : flags[t] & ~Isl.F_PIN;
    const was = sel.some((t) => I.flags[t] & Isl.F_PIN);
    flagsOnly(flags, pin ? 'Pin islands' : 'Unpin islands');
    ctx.status(pin ? 'Pinned: UV edits leave these islands where they are (Alt+P unpins)' : was ? 'Unpinned the selected islands' : 'None of the selected islands were pinned');
  }
  function packAll() {
    const tp = topo(), uv = uvArr().slice(), flags = I.flags.slice();
    const d = Isl.densityTargets(tp, uv, flags, null);
    const k = Isl.repackMovable(tp, uv, flags, (d.layout || d.fit) * 4, [], 0.005);
    if (!k) { ctx.status('Nothing to pack (or no room around the pinned islands)', true); return; }
    applyUVEdit(uv, flags, 'Pack');
    ctx.status('Packed' + (flags.some((f) => f & Isl.F_PIN) ? ' around the pinned islands' : ''));
  }
  function restackAll() {
    const uv = uvArr().slice(), flags = I.flags.slice();
    const n = Isl.restack(uv, Isl.mirrorLinks(topo(), flags.slice(), [uv]));
    if (!n) { ctx.status('Every mirrored half is on its other half'); return; }
    applyUVEdit(uv, flags, 'Re-stack mirrored halves');
    ctx.status(`${n} mirrored triangles are stacked on their other half again`);
  }
  function unstackSel() {
    const c = info(), tp = topo();
    const sel = selectedTris();
    const linked = Isl.withLinked(tp, c.links, sel);
    const copies = linked.filter((t) => c.links.src[3 * t] >= 0 && !(I.flags[t] & Isl.F_PIN));
    if (!copies.length) { ctx.status('No mirrored halves in the selection', true); return; }
    const uv = uvArr().slice(), flags = I.flags.slice();
    for (const t of copies) flags[t] &= ~Isl.F_MIRROR;
    // the copies' own islands (cut out of the stack) move into free space
    const isl = Isl.uvIslands(tp, uv), groups = new Map();
    for (const t of copies) { const i = isl.id[t]; if (!groups.has(i)) groups.set(i, []); groups.get(i).push(t); }
    if (!Isl.placeInFreeSpace(uv, flags, tp.T, [...groups.values()], 0.005, true)) {
      const d = Isl.densityTargets(tp, uv, flags, null);
      Isl.repackMovable(tp, uv, flags, d.layout || d.fit);
    }
    applyUVEdit(uv, flags, 'Unstack mirrored halves');
    ctx.status('The mirrored half has its own place now: each side can be painted separately');
  }

  // ---------------------------------------------------------------- UV view: islands mode
  const uvc = ctx.uvCanvas;
  const toUV = (e) => { const r = uvc.getBoundingClientRect(); return { u: (e.clientX - r.left) / r.width, v: 1 - (e.clientY - r.top) / r.height, x: e.clientX, y: e.clientY }; };
  function triAt(p) {
    const uv = uvArr(), T = I.flags.length;
    let hit = -1, prim = -1;
    for (let t = 0; t < T; t++) {
      const o = 6 * t;
      const ax = uv[o], ay = uv[o + 1], bx = uv[o + 2], by = uv[o + 3], cx = uv[o + 4], cy = uv[o + 5];
      if (p.u < Math.min(ax, bx, cx) || p.u > Math.max(ax, bx, cx) || p.v < Math.min(ay, by, cy) || p.v > Math.max(ay, by, cy)) continue;
      const d1 = (bx - ax) * (p.v - ay) - (by - ay) * (p.u - ax), d2 = (cx - bx) * (p.v - by) - (cy - by) * (p.u - bx), d3 = (ax - cx) * (p.v - cy) - (ay - cy) * (p.u - cx);
      if ((d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0)) {
        hit = t;
        if (!(I.flags[t] & Isl.F_COPY)) prim = t;
      }
    }
    return prim >= 0 ? prim : hit;
  }
  const islandOf = (t) => { const c = info(); return c.per[c.isl.id[t]].tris; };
  let press = null;
  uvc.addEventListener('pointerdown', (e) => {
    if (I.uvMode !== 'islands') return;
    e.stopImmediatePropagation();
    closeMenu();
    const p = toUV(e);
    if (e.button === 2) { press = { right: true, p }; return; }
    if (e.button !== 0) return;
    const t = triAt(p);
    const plain = !e.shiftKey && !e.ctrlKey && !e.metaKey;
    press = { p, t, plain, box: t < 0, onSel: t >= 0 && I.sel[t] };
    if (t >= 0 && !(press.onSel && plain)) {
      setSelection(islandOf(t), e.ctrlKey || e.metaKey ? 'remove' : e.shiftKey ? 'add' : 'set');
      press.t = -1;  // already selected: a release does nothing more
    }
    if (t < 0 && plain) setSelection([], 'set');
    uvc.setPointerCapture(e.pointerId);
  }, true);
  uvc.addEventListener('pointermove', (e) => {
    if (I.uvMode !== 'islands' || !press || press.right) return;
    e.stopImmediatePropagation();
    const p = toUV(e);
    if (press.box) { I.drag = { box: [press.p, p] }; ctx.requestUV(); return; }
    if (!press.plain) return;
    if (!I.drag && Math.hypot(p.x - press.p.x, p.y - press.p.y) > 4) {
      const tris = Isl.withLinked(topo(), info().links, selectedTris());
      const movable = tris.filter((t) => !(I.flags[t] & Isl.F_PIN));
      if (!movable.length) { ctx.status('Pinned: Alt+P (or right-click > Unpin islands) to move them', true); press = null; return; }
      if (movable.length < tris.length) ctx.status('Pinned islands in the selection stay where they are');
      I.drag = { tris: movable, du: 0, dv: 0 };
    }
    if (I.drag && I.drag.tris) { I.drag.du = p.u - press.p.u; I.drag.dv = p.v - press.p.v; ctx.requestUV(); }
  }, true);
  const release = (e) => {
    if (I.uvMode !== 'islands' || !press) return;
    e.stopImmediatePropagation();
    const pr = press;
    press = null;
    const d = I.drag;
    I.drag = null;
    if (pr.right) {
      const t = triAt(pr.p);
      if (t >= 0 && !I.sel[t]) setSelection(islandOf(t), 'set');
      openMenu(pr.p.x, pr.p.y);
      return;
    }
    if (d && d.box) {
      const [a, b] = d.box, u0 = Math.min(a.u, b.u), u1 = Math.max(a.u, b.u), v0 = Math.min(a.v, b.v), v1 = Math.max(a.v, b.v);
      const c = info(), hit = [];
      for (const p of c.per) {
        const cu = (p.x0 + p.x1) / 2, cv = (p.y0 + p.y1) / 2;
        if (cu >= u0 && cu <= u1 && cv >= v0 && cv <= v1) hit.push(...p.tris);
      }
      setSelection(hit, e.ctrlKey ? 'remove' : 'add');
      ctx.requestUV();
      return;
    }
    if (d && d.tris) {
      const uv = uvArr().slice();
      for (const t of d.tris) for (let k = 0; k < 3; k++) { uv[2 * (3 * t + k)] += d.du; uv[2 * (3 * t + k) + 1] += d.dv; }
      applyUVEdit(uv, I.flags.slice(), 'Move UVs');
      return;
    }
    if (pr.t >= 0 && pr.onSel) setSelection(islandOf(pr.t), 'set');  // a click on the selection selects just this island
    ctx.requestUV();
  };
  uvc.addEventListener('pointerup', release, true);
  uvc.addEventListener('pointercancel', release, true);

  // ---------------------------------------------------------------- context menu
  const menu = document.createElement('div');
  menu.id = 'uvMenu';
  menu.hidden = true;
  document.body.appendChild(menu);
  function closeMenu() { menu.hidden = true; }
  window.addEventListener('pointerdown', (e) => { if (!menu.contains(e.target)) closeMenu(); });
  function openMenu(x, y) {
    const c = info();
    const sel = selectedTris();
    const any = sel.length > 0;
    let pinnedSel = false, linked = false;
    for (const t of sel) { if (I.flags[t] & Isl.F_PIN) pinnedSel = true; if (c.links.src[3 * t] >= 0) linked = true; }
    if (!linked && any) linked = Isl.withLinked(topo(), c.links, sel).length > sel.length;
    const rot = (a) => (u, v) => [Math.cos(a) * u - Math.sin(a) * v, Math.sin(a) * u + Math.cos(a) * v];
    const items = [
      [`${any ? sel.length + ' triangles selected' : 'Nothing selected'}${pinnedSel ? ', pinned' : ''}${linked ? ', mirrored' : ''}`, null, 'head'],
      ['Pin islands', () => pinSel(true), any, 'P'],
      ['Unpin islands', () => pinSel(false), any && pinnedSel, 'Alt+P'],
      '-',
      ['Rotate 90° clockwise', () => transformSel('Rotate UVs', rot(-Math.PI / 2)), any],
      ['Rotate 90° counter-clockwise', () => transformSel('Rotate UVs', rot(Math.PI / 2)), any],
      ['Rotate 180°', () => transformSel('Rotate UVs', rot(Math.PI)), any],
      ['Flip horizontal', () => transformSel('Flip UVs', (u, v) => [-u, v]), any],
      ['Flip vertical', () => transformSel('Flip UVs', (u, v) => [u, -v]), any],
      '-',
      ['Orient upright (model up = texture up)', () => islandOp('Oriented upright', (tp, uv, ch) => Isl.orientChart(tp, uv, ch, 'upright')), any],
      ['Orient along the longest side', () => islandOp('Oriented', (tp, uv, ch) => Isl.orientChart(tp, uv, ch, 'longest')), any],
      ['Texel density: as the Island brush', () => {
        const tp = topo(), ex = new Uint8Array(tp.T);
        for (const t of sel) ex[t] = 1;
        const d = densityTarget(tp, uvArr(), ex);
        islandOp('Texel density set', (tp2, uv, ch) => Isl.scaleChart(tp2, uv, ch, d));
      }, any],
      ['Move into free space', () => {
        const { charts, pinned } = selIslands(true);
        if (!charts.length) return noneMsg(pinned);
        const uv = uvArr().slice(), flags = I.flags.slice();
        if (!Isl.placeInFreeSpace(uv, flags, flags.length, charts, 0.005, true)) { ctx.status('Not enough free UV space', true); return; }
        applyUVEdit(uv, flags, 'Move into free space');
        ctx.status('Moved into free space');
      }, any],
      ['Make island from the selection', makeIsland, any, 'Enter'],
      ['Relax (remove stretch, stay in place)', () => {
        const tris = selectedTris();
        const uv = uvArr().slice(), flags = I.flags.slice();
        const n = Isl.relaxIslands(topo(), uv, flags, tris);
        if (!n) { ctx.status('Nothing to relax (pinned islands stay as they are)', true); return; }
        applyUVEdit(uv, flags, 'Relax');
        ctx.status(`Relaxed ${n} island${n === 1 ? '' : 's'}: the stretch is gone, they stay in place`);
      }, any],
      '-',
      ['Select the other half', () => setSelection(Isl.withLinked(topo(), c.links, sel), 'add'), linked],
      ['Re-stack mirrored halves', restackAll, c.unstacked > 0],
      ['Unstack (make independent)', unstackSel, linked],
      '-',
      ['Select all', () => setSelection([...Array(I.sel.length).keys()], 'set'), true],
      ['Select none', () => setSelection([], 'set'), any],
      ['Select pinned islands', () => setSelection([...Array(I.sel.length).keys()].filter((t) => I.flags[t] & Isl.F_PIN), 'set'), I.flags.some((f) => f & Isl.F_PIN)],
      ['Pack all islands', packAll, true],
      ['Unpin every island', () => { const f = I.flags.slice(); for (let t = 0; t < f.length; t++) f[t] &= ~Isl.F_PIN; flagsOnly(f, 'Unpin every island'); ctx.status('No islands are pinned now'); }, I.flags.some((f) => f & Isl.F_PIN)],
    ];
    menu.innerHTML = '';
    for (const it of items) {
      if (it === '-') { menu.appendChild(document.createElement('hr')); continue; }
      const [label, fn, enabled, key] = it;
      const d = document.createElement('div');
      d.className = enabled === 'head' ? 'head' : 'item' + (enabled ? '' : ' off');
      d.innerHTML = `<span>${label}</span>${key ? `<kbd>${key}</kbd>` : ''}`;
      if (fn && enabled) d.addEventListener('click', () => { closeMenu(); fn(); });
      menu.appendChild(d);
    }
    menu.hidden = false;
    const r = menu.getBoundingClientRect();
    menu.style.left = Math.min(x, window.innerWidth - r.width - 4) + 'px';
    menu.style.top = Math.min(y, window.innerHeight - r.height - 4) + 'px';
  }
  uvc.addEventListener('contextmenu', (e) => e.preventDefault());

  // ---------------------------------------------------------------- drawing on the UV view
  function drawOverlay(g, N) {
    const showSel = I.uvMode === 'islands' || state.tool === 'island';
    const uv = uvArr();
    const X = (u) => u * N, Y = (v) => (1 - v) * N;
    if (showSel) {
      g.fillStyle = 'rgba(255, 120, 30, 0.35)';
      g.beginPath();
      for (let t = 0; t < I.sel.length; t++) {
        if (!I.sel[t]) continue;
        const o = 6 * t;
        g.moveTo(X(uv[o]), Y(uv[o + 1])); g.lineTo(X(uv[o + 2]), Y(uv[o + 3])); g.lineTo(X(uv[o + 4]), Y(uv[o + 5])); g.closePath();
      }
      g.fill();
    }
    if (I.flags.length && (showSel || I.flags.some((f) => f & (Isl.F_PIN | Isl.F_COPY)))) {
      const c = info();
      for (const p of c.per) {
        if (!(p.pinned || p.stacked) || !isFinite(p.x0)) continue;
        const ax = X(p.x0), ay = Y(p.y1), bx = X(p.x1), by = Y(p.y0);
        if (p.pinned) {
          g.strokeStyle = 'rgba(110, 180, 255, 0.8)';
          g.lineWidth = 1;
          g.strokeRect(ax - 2, ay - 2, bx - ax + 4, by - ay + 4);
          g.strokeStyle = '#6eb4ff'; g.lineWidth = 2;
          g.beginPath(); g.moveTo(bx, ay); g.lineTo(bx - 7, ay + 7); g.stroke();
          g.fillStyle = '#6eb4ff';
          g.beginPath(); g.arc(bx, ay, 4, 0, Math.PI * 2); g.fill();
        }
        if (p.stacked && bx - ax > 22 && by - ay > 14) {
          g.fillStyle = 'rgba(20, 50, 30, 0.85)'; g.fillRect(ax + 2, ay + 2, 18, 12);
          g.fillStyle = '#96ebaa'; g.font = '10px sans-serif'; g.fillText('x2', ax + 5, ay + 11);
        }
      }
    }
    const d = I.drag;
    if (d && d.tris) {
      g.strokeStyle = 'rgba(255, 210, 140, 0.9)';
      g.lineWidth = 1;
      g.beginPath();
      for (const t of d.tris) {
        const o = 6 * t;
        g.moveTo(X(uv[o] + d.du), Y(uv[o + 1] + d.dv)); g.lineTo(X(uv[o + 2] + d.du), Y(uv[o + 3] + d.dv)); g.lineTo(X(uv[o + 4] + d.du), Y(uv[o + 5] + d.dv)); g.closePath();
      }
      g.stroke();
    }
    if (d && d.box) {
      const [a, b] = d.box;
      g.strokeStyle = 'rgba(232, 133, 44, 0.9)'; g.fillStyle = 'rgba(232, 133, 44, 0.15)';
      const x = X(Math.min(a.u, b.u)), y = Y(Math.max(a.v, b.v)), w = Math.abs(a.u - b.u) * N, h = Math.abs(a.v - b.v) * N;
      g.fillRect(x, y, w, h); g.strokeRect(x, y, w, h);
    }
    if (I.uvMode === 'islands' && I.flags.length) {
      const c = info();
      if (c.unstacked) { g.fillStyle = 'rgba(120,40,20,0.85)'; g.fillRect(4, N - 22, N - 8, 18); g.fillStyle = '#ffd2aa'; g.font = '11px sans-serif'; g.fillText(`${c.unstacked} mirrored triangles moved off their other half: right-click > Re-stack`, 8, N - 9); }
    }
  }

  // ---------------------------------------------------------------- side panel
  function updatePanel() {
    let n = 0, pinned = 0;
    for (let t = 0; t < I.sel.length; t++) if (I.sel[t]) { n++; if (I.flags[t] & Isl.F_PIN) pinned++; }
    const el = $('islSelCount');
    if (el) el.textContent = n ? `${n.toLocaleString()} triangles selected` + (pinned ? `, ${pinned.toLocaleString()} pinned` : '') : 'Paint over the model to select a region';
    for (const [id, off] of [['islMake', !n], ['islPinSel', !n], ['islUnpinSel', !pinned]]) { const b = $(id); if (b) b.disabled = off; }
  }
  $('islMake').addEventListener('click', makeIsland);
  $('islClear').addEventListener('click', () => setSelection([], 'set'));
  $('islPinSel').addEventListener('click', () => pinSel(true));
  $('islUnpinSel').addEventListener('click', () => pinSel(false));
  $('islAll').addEventListener('click', selectAll);
  $('islGrow').addEventListener('click', () => growShrink(1));
  $('islShrink').addEventListener('click', () => growShrink(-1));
  $('islOrient').addEventListener('change', (e) => { I.orient = e.target.value; });
  $('islDensity').addEventListener('change', (e) => { I.density = e.target.value; $('islCustomRow').hidden = I.density !== 'custom'; });
  $('islCustom').addEventListener('change', (e) => { I.custom = Math.max(1, parseFloat(e.target.value) || 1024); });
  for (const [id, bit] of [['islSymX', 1], ['islSymY', 2], ['islSymZ', 4]])
    $(id).addEventListener('change', (e) => { I.axes = e.target.checked ? I.axes | bit : I.axes & ~bit; });
  $('islPin').addEventListener('change', (e) => { I.pin = e.target.checked; });
  $('islKeep').addEventListener('change', (e) => { I.keepPaint = e.target.checked; });
  $('selUVMode').addEventListener('change', (e) => { I.uvMode = e.target.value; closeMenu(); ctx.requestUV(); });

  function selectAll() {
    I.sel.fill(1);
    syncSel();
    updatePanel();
  }
  // a ring of triangles (sharing a vertex with the region) more or less
  function growShrink(dir) {
    const tp = topo(), T = tp.T, next = I.sel.slice();
    for (let t = 0; t < T; t++) {
      if ((dir > 0) === !!I.sel[t]) continue;
      let touch = false;
      for (let k = 0; k < 3 && !touch; k++)
        for (const g of tp.trisAt(tp.canon[3 * t + k])) if ((dir > 0) === !!I.sel[g]) { touch = true; break; }
      if (touch) next[t] = dir > 0 ? 1 : 0;
    }
    I.sel = next;
    syncSel();
    updatePanel();
  }
  function onKey(e, uvHovered) {
    const k = e.key.toLowerCase();
    if (state.tool === 'island' && !uvHovered && !e.ctrlKey && !e.metaKey && k === 'a') {
      e.preventDefault();
      if (e.altKey) setSelection([], 'set'); else selectAll();
      return true;
    }
    if (state.tool === 'island' && (e.ctrlKey || e.metaKey) && ['=', '+', '-', '_'].includes(e.key)) {
      e.preventDefault();
      growShrink(e.key === '-' || e.key === '_' ? -1 : 1);
      return true;
    }
    if (e.key === 'Enter' && state.tool === 'island') { e.preventDefault(); makeIsland(); return true; }
    if (k === 'p' && (uvHovered && I.uvMode === 'islands')) { e.preventDefault(); pinSel(!e.altKey); return true; }
    // (0.37.1) Alt+P unpins in the 3D view too (P there is the Fill tool)
    if (k === 'p' && e.altKey && !uvHovered && state.tool === 'island') { e.preventDefault(); pinSel(false); return true; }
    return false;
  }

  updatePanel();
  return {
    onGeometry, brushDown, brushMove, brushUp, drawOverlay, onKey, makeIsland, applyUVEdit, info, pinSel,
    setSelection, selectedTris, packAll, restackAll, selectAll, growShrink, pickTri, topo: () => topo(),
    clearForNewUVs() { I.flags.fill(0); I.version++; I.cache = null; syncSel(); updatePanel(); },
    setFlags(f) { if (f.length === I.flags.length) { I.flags.set(f); I.version++; I.cache = null; syncSel(); updatePanel(); } },
  };
}
