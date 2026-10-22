// Patina Web - UI, scene and input.
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { OBJLoader } from 'three/addons/loaders/OBJLoader.js';
import { FBXLoader } from 'three/addons/loaders/FBXLoader.js';
import { STLLoader } from 'three/addons/loaders/STLLoader.js';
import { ThreeMFLoader } from 'three/addons/loaders/3MFLoader.js';
import { GLTFExporter } from 'three/addons/exporters/GLTFExporter.js';
import { RoomEnvironment } from 'three/addons/environments/RoomEnvironment.js';
import { PaintEngine, BLENDS } from './engine.js';
import { libraryMaterials, findMaterial, materialTextures, cachedTextures, addCustomMaterial, customMaterials } from './library.js';
import { unzipSync, zlibSync } from 'three/addons/libs/fflate.module.js';
import { mergeScene, normalize, uvReport, atlasUnwrap, computeTangents, normalizeUVTiles } from './geometry.js';
import { makeZip, crc32 } from './zip.js';
import { setupIslands } from './islandui.js';
import * as Isl from './islands.js';
import { setupGizmo } from './gizmo.js';

const $ = (id) => document.getElementById(id);

// ------------------------------------------------------------------ renderer & scene
const canvas = $('canvas');
let renderer;
try {
  renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: true });
} catch (e) {
  $('statusText').textContent = 'WebGL 2 is not available in this browser.';
  throw e;
}
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
renderer.toneMapping = THREE.NeutralToneMapping;
renderer.outputColorSpace = THREE.SRGBColorSpace;
const gl = renderer.getContext();
if (!(gl instanceof WebGL2RenderingContext)) $('statusText').textContent = 'WebGL 2 required';

const scene = new THREE.Scene();
const pmrem = new THREE.PMREMGenerator(renderer);
scene.environment = pmrem.fromScene(new RoomEnvironment(), 0.03).texture;
scene.environmentIntensity = 0.55;
const key = new THREE.DirectionalLight(0xffffff, 1.6);
key.position.set(-2, 3, 2.5);
scene.add(key);

const camera = new THREE.PerspectiveCamera(35, 1, 0.02, 100);
camera.position.set(0, 0.4, 4.2);
const controls = new OrbitControls(camera, canvas);
controls.enableDamping = false;
controls.mouseButtons = { LEFT: -1, MIDDLE: THREE.MOUSE.ROTATE, RIGHT: THREE.MOUSE.PAN };
controls.zoomSpeed = 1.2;
// the X/Y/Z axes in the top right corner (gizmo.js): click one to look along it
const gizmo = setupGizmo({ container: $('viewport'), canvas, camera, controls, avoid: $('uvPanel') });

const engine = new PaintEngine(renderer);
const displayMat = new THREE.MeshStandardMaterial({ roughness: 1, metalness: 1 });
const selShow = { value: 0 };  // Island brush: tint the selected triangles
displayMat.onBeforeCompile = (sh) => {
  sh.uniforms.uSelShow = selShow;
  sh.vertexShader = sh.vertexShader.replace('#include <common>', '#include <common>\nattribute float aSel;\nvarying float vSel;')
    .replace('#include <begin_vertex>', '#include <begin_vertex>\n  vSel = aSel;');
  // the painted color map holds sRGB values (like the desktop app): decode before lighting
  sh.fragmentShader = sh.fragmentShader.replace('#include <common>', '#include <common>\nvarying float vSel;\nuniform float uSelShow;')
    .replace('#include <map_fragment>', `
#ifdef USE_MAP
  vec4 sampledDiffuseColor = texture2D( map, vMapUv );
  vec3 cs = sampledDiffuseColor.rgb;
  sampledDiffuseColor.rgb = mix(cs / 12.92, pow((cs + 0.055) / 1.055, vec3(2.4)), step(0.04045, cs));
  diffuseColor *= sampledDiffuseColor;
#endif
  // vSel: 1 selected, + 2 pinned (blue stripes, as on the desktop app)
  diffuseColor.rgb = mix(diffuseColor.rgb, vec3(1.0, 0.42, 0.08), mod(vSel + 0.25, 2.0) > 0.75 ? 0.6 * uSelShow : 0.0);
  if (vSel > 1.5 && fract((gl_FragCoord.x + gl_FragCoord.y) / 14.0) < 0.36) diffuseColor.rgb = mix(diffuseColor.rgb, vec3(0.16, 0.46, 1.0), 0.6 * uSelShow);`);  // (linear: the sRGB 110, 180, 255)
};
const mesh = new THREE.Mesh(new THREE.BufferGeometry(), displayMat);
scene.add(mesh);

// ------------------------------------------------------------------ state
const state = {
  tool: 'paint',
  brush: { size: 60, hardness: 0.7, opacity: 1, flow: 0.6, spacing: 0.12, tip: 0, pressSize: true, pressFlow: false, through: false,
    sym: 0, blurRadius: 6, fillMode: 'island' },
  channels: { color: true, rm: true, height: true },
  material: { id: '', isFlat: true, flat: { color: '#c23b22', roughness: 0.45, metallic: 0, height: 0.5 },
    scale: 1, defaultScale: 1, mapping: 0, tint: '#ffffff', heightAmt: 1, roughOffset: 0 },
  model: { name: '', convention: 'gl', restore: new THREE.Matrix4(), report: null },
  lastEnd: null,
  painting: null,
  uvVisible: false,
  uvDirty: true,
  uvMap: 'color',
};

function status(t, warn) {
  const s = $('statusText');
  s.textContent = t;
  s.className = warn ? 'warn' : '';
}

// ------------------------------------------------------------------ Island brush & UV islands
const isl = setupIslands({
  renderer, camera, canvas, mesh, engine, state, $, status, uvCanvas: $('uvCanvas'),
  cssSize: () => cssSize(),
  afterLayout: () => {
    computeTangents(mesh.geometry);
    engine.rasterUV();
    engine.dirty = true;
    state.model.report = uvReport(mesh.geometry);
    uvWire = null;
    state.uvDirty = true;
  },
  updateUndoButtons: () => updateUndoButtons(),
  requestUV: () => { state.uvDirty = true; },
});
$('selUVMode').addEventListener('change', () => { selShow.value = state.tool === 'island' || state.island.uvMode === 'islands' ? 1 : 0; });

// ------------------------------------------------------------------ model loading
function setGeometry(geometry, name, convention, hadUVs = true, autoFix = false) {
  state.model.restore = normalize(geometry);
  state.model.name = name;
  state.model.convention = convention;
  let note = '';
  const movedTiles = hadUVs ? normalizeUVTiles(geometry) : 0;
  if (movedTiles) note = ` · moved ${movedTiles.toLocaleString()} triangles from other UV tiles into 0-1`;
  let report = uvReport(geometry);
  const bad = !hadUVs || report.overlap > 0.02 || report.outside > 0.01 || report.degenerate > 0.2;
  if (bad && (autoFix || !hadUVs)) {
    atlasUnwrap(geometry, 2048);
    report = uvReport(geometry);
    note += hadUVs ? ' · UVs overlapped: generated unique UVs' : ' · no UVs in the file: generated unique UVs';
  }
  state.model.report = report;
  computeTangents(geometry);
  // convexity per corner, for the Edges / Cavities smart masks
  geometry.setAttribute('aCurv', new THREE.BufferAttribute(Isl.vertexCurvature(new Isl.Topo(geometry)), 1));
  isl.onGeometry(geometry);
  mesh.geometry.dispose();
  mesh.geometry = geometry;
  engine.setMesh(geometry, parseInt($('selRes').value, 10));
  applyMaps();
  frame();
  refreshLayers();
  state.lastEnd = null;
  state.uvDirty = true;
  uvWire = null;
  const warn = bad && !(autoFix || !hadUVs);
  status(`${name}: ${report.triangles.toLocaleString()} triangles · UV coverage ${(report.coverage * 100).toFixed(0)}%` +
    (warn ? ` · UVs overlap ${(report.overlap * 100).toFixed(0)}% / outside 0-1 ${(report.outside * 100).toFixed(0)}% - paint will repeat there: use "Unique UVs"` : note), warn);
}

async function loadSample(name) {
  status('Loading sample ' + name + '…');
  if (!window.PatinaSamples || !window.PatinaSamples[name]) {
    await new Promise((res, rej) => {
      const sc = document.createElement('script');
      sc.src = 'samples/' + name + '.js';
      sc.onload = res;
      sc.onerror = () => rej(new Error('sample not found (samples/' + name + '.js)'));
      document.head.appendChild(sc);
    });
  }
  const b64 = window.PatinaSamples[name].split(',')[1];
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  const file = new File([bytes], name + '.glb');
  await loadFile(file);
}

function primitive(kind) {
  if (kind === 'suzanne') { loadSample('Suzanne').catch((e) => status(e.message, true)); return; }
  let g;
  if (kind === 'sphere') g = new THREE.SphereGeometry(1, 128, 64);
  else if (kind === 'cube') g = new THREE.BoxGeometry(1.4, 1.4, 1.4, 8, 8, 8);
  else if (kind === 'torus') g = new THREE.TorusGeometry(1, 0.4, 64, 160);
  else if (kind === 'cylinder') g = new THREE.CylinderGeometry(0.8, 0.8, 1.8, 96, 8);
  else g = new THREE.TorusKnotGeometry(0.8, 0.28, 320, 48);
  g = g.toNonIndexed();
  setGeometry(g, kind[0].toUpperCase() + kind.slice(1), 'gl', true, true);
}

async function loadFile(file) {
  const name = file.name.replace(/\.[^.]+$/, '');
  const ext = file.name.split('.').pop().toLowerCase();
  if (ext === 'patinaweb') return openProject(file);
  if (['png', 'jpg', 'jpeg', 'webp'].includes(ext)) return imageMaterial(file);
  status('Loading ' + file.name + '…');
  try {
    let root, convention;
    if (ext === 'obj') {
      root = new OBJLoader().parse(await file.text());
      convention = 'gl';
    } else if (ext === 'glb' || ext === 'gltf') {
      const buf = await file.arrayBuffer();
      const gltf = await new Promise((res, rej) => new GLTFLoader().parse(buf, '', res, rej));
      root = gltf.scene;
      convention = 'gltf';
    } else if (ext === 'fbx') {
      root = new FBXLoader().parse(await file.arrayBuffer(), '');
      convention = 'gl';
    } else if (ext === 'stl') {
      // 3D printing: Z up in millimeters (the view frames the model, so only the axes matter)
      const g = new STLLoader().parse(await file.arrayBuffer());
      g.rotateX(-Math.PI / 2);
      root = new THREE.Mesh(g);
      convention = 'gl';
    } else if (ext === '3mf') {
      root = new ThreeMFLoader().parse(await file.arrayBuffer());
      root.rotation.set(-Math.PI / 2, 0, 0);  // 3MF is Z up
      root.updateMatrixWorld(true);
      convention = 'gl';
    } else {
      throw new Error('unsupported file type .' + ext + ' (use .glb, .gltf, .obj, .fbx, .stl or .3mf)');
    }
    const { geometry, hadUVs } = mergeScene(root);
    setGeometry(geometry, name, convention, hadUVs, false);
  } catch (e) {
    console.error(e);
    status('Could not open ' + file.name + ': ' + e.message, true);
  }
}

function frame() {
  const r = 1.0;
  const fov = THREE.MathUtils.degToRad(camera.fov);
  const dist = r / Math.sin(fov / 2) * 1.08;
  const dir = camera.position.clone().sub(controls.target).normalize();
  if (dir.lengthSq() < 0.5) dir.set(0, 0.1, 1).normalize();
  controls.target.set(0, 0, 0);
  camera.position.copy(dir.multiplyScalar(dist));
  camera.near = dist * 0.02;
  camera.far = dist * 20;
  camera.updateProjectionMatrix();
  controls.update();
}

function applyMaps() {
  // atlas UVs put unrelated triangles side by side: mipmaps would blend them into visible seams
  const atlas = !!mesh.geometry.userData.atlas;
  for (const t of [engine.finalColor, engine.finalORM, engine.finalNormal]) {
    t.texture.minFilter = atlas ? THREE.LinearFilter : THREE.LinearMipmapLinearFilter;
    t.texture.generateMipmaps = !atlas;
    t.texture.needsUpdate = true;
  }
  displayMat.map = engine.finalColor.texture;
  displayMat.roughnessMap = engine.finalORM.texture;
  displayMat.metalnessMap = engine.finalORM.texture;
  displayMat.normalMap = engine.finalNormal.texture;
  displayMat.needsUpdate = true;
}

// ------------------------------------------------------------------ painting input
function cssSize() {
  const r = canvas.getBoundingClientRect();
  return { w: r.width, h: r.height, left: r.left, top: r.top };
}

function strokeOpts(uvSpace) {
  const l = engine.layers[engine.selected];
  return {
    tool: state.tool, hardness: state.brush.hardness, tip: state.brush.tip, through: state.brush.through,
    symmetry: state.brush.sym, blurRadius: state.brush.blurRadius,
    opacity: state.brush.opacity, channels: { ...state.channels }, material: { ...state.material, flat: { ...state.material.flat } },
    uvSpace, fillMask: l && l.kind !== 'paint',
  };
}

function pressureOf(e) {
  return e.pointerType === 'pen' ? Math.max(0.02, e.pressure || 0) : 1;
}

function dabsAlong(from, to, size, pressure) {
  const b = state.brush;
  const radius = (size / 2) * (b.pressSize ? pressure : 1);
  const strength = b.flow * (b.pressFlow ? pressure : 1);
  const step = Math.max(1, b.spacing * size);
  const out = [];
  const dx = to.x - from.x, dy = to.y - from.y;
  const len = Math.hypot(dx, dy);
  let t = from.carry ?? 0;
  if (len === 0 && t === 0) return { dabs: [{ x: to.x, y: to.y, radius, strength }], carry: step };
  while (t <= len) {
    const k = len ? t / len : 0;
    out.push({ x: from.x + dx * k, y: from.y + dy * k, radius, strength });
    t += step;
  }
  return { dabs: out, carry: t - len };
}

function startStroke(pt, uvSpace, e) {
  if (!engine.layers.length) return false;
  if (state.material.id && !state.material.isFlat && !cachedTextures(state.material.id)) {
    status('Material is still loading…', true);
    return false;
  }
  const v = cssSize();
  const ok = engine.beginStroke({ camera, cssWidth: v.w, cssHeight: v.h }, strokeOpts(uvSpace));
  if (!ok) { status('Select a visible layer to paint on', true); return false; }
  const size = uvSpace ? uvBrushTexels() : state.brush.size;
  const p = pressureOf(e);
  let r;
  if (e.shiftKey && state.lastEnd && state.lastEnd.uvSpace === uvSpace) r = dabsAlong({ ...state.lastEnd, carry: 0 }, pt, size, p);
  else r = dabsAlong({ ...pt, carry: 0 }, pt, size, p);
  state.painting = { last: { ...pt, carry: r.carry }, uvSpace, pending: r.dabs };
  return true;
}

function moveStroke(pt, e) {
  const s = state.painting;
  if (!s) return;
  const size = s.uvSpace ? uvBrushTexels() : state.brush.size;
  const r = dabsAlong(s.last, pt, size, pressureOf(e));
  s.pending.push(...r.dabs);
  s.last = { ...pt, carry: r.carry };
}

function endStroke() {
  const s = state.painting;
  if (!s) return;
  flushDabs();
  engine.endStroke();
  state.lastEnd = { x: s.last.x, y: s.last.y, uvSpace: s.uvSpace };
  state.painting = null;
  state.uvDirty = true;
  updateUndoButtons();
}

function flushDabs() {
  const s = state.painting;
  if (s && s.pending.length) {
    engine.addDabs(s.pending);
    s.pending = [];
    state.uvDirty = true;
  }
}

canvas.addEventListener('pointerdown', (e) => {
  if (e.button !== 0 || e.altKey) return;
  const v = cssSize();
  const pt = { x: e.clientX - v.left, y: e.clientY - v.top };
  if (state.tool === 'island') { isl.brushDown(pt, e); canvas.setPointerCapture(e.pointerId); return; }
  if (state.tool === 'pick' || state.quickPick) { if (state.quickPick) state.pickedDuringHold = true; pickAt(pt); return; }
  if (state.tool === 'fill') { fillAt(pt, e.ctrlKey || e.metaKey); return; }
  if (startStroke(pt, false, e)) canvas.setPointerCapture(e.pointerId);
});
canvas.addEventListener('pointermove', (e) => {
  const v = cssSize();
  const pt = { x: e.clientX - v.left, y: e.clientY - v.top };
  moveCursor(e.clientX, e.clientY, state.brush.size * (state.painting && state.brush.pressSize ? pressureOf(e) : 1));
  if (state.tool === 'island') isl.brushMove(pt, e);
  if (state.painting && !state.painting.uvSpace) {
    const evs = e.getCoalescedEvents ? e.getCoalescedEvents() : [e];
    for (const ce of evs.length ? evs : [e]) moveStroke({ x: ce.clientX - v.left, y: ce.clientY - v.top }, ce);
  }
  void pt;
});
for (const ev of ['pointerup', 'pointercancel']) canvas.addEventListener(ev, () => { endStroke(); isl.brushUp(); });
canvas.addEventListener('pointerleave', () => { $('cursor').style.display = 'none'; });
canvas.addEventListener('contextmenu', (e) => e.preventDefault());
$('viewport').addEventListener('wheel', (e) => {
  if (!e.ctrlKey) return;
  e.preventDefault();
  e.stopPropagation();
  setBrushSize(state.brush.size * (e.deltaY < 0 ? 1.1 : 1 / 1.1));
}, { capture: true, passive: false });

// ---- polygon fill: the triangle under the cursor, its UV island, its connected part or everything
let partCache = null;
function connectedPart(tp, t0) {
  if (!partCache || partCache.topo !== tp) {
    const comp = new Int32Array(tp.T).fill(-1);
    let n = 0;
    for (let s = 0; s < tp.T; s++) {
      if (comp[s] >= 0) continue;
      const q = [s];
      comp[s] = n;
      for (let i = 0; i < q.length; i++)
        for (let k = 0; k < 3; k++)
          for (const g of tp.trisAt(tp.canon[3 * q[i] + k])) if (comp[g] < 0) { comp[g] = n; q.push(g); }
      n++;
    }
    partCache = { topo: tp, comp };
  }
  const c = partCache.comp[t0], out = [];
  for (let t = 0; t < tp.T; t++) if (partCache.comp[t] === c) out.push(t);
  return out;
}
function fillAt(pt, erase) {
  const t0 = isl.pickTri(pt);
  if (t0 < 0) { status('Click on the model to fill', true); return; }
  const tp = isl.topo();
  let tris;
  const mode = state.brush.fillMode;
  if (mode === 'tri') tris = [t0];
  else if (mode === 'island') { const info = isl.info(); tris = info.lists[info.isl.id[t0]]; }
  else if (mode === 'part') tris = connectedPart(tp, t0);
  else tris = Array.from({ length: tp.T }, (_, i) => i);
  // mirror symmetry: the mirrored triangles too
  const sym = state.brush.sym;
  if (sym && mode !== 'all') {
    const set = new Set(tris);
    for (const t of tris)
      for (let s = 1; s < 8; s++) if ((s & sym) === s) for (const g of Isl.mirrorTris(tp, t, s)) set.add(g);
    if (mode === 'island' || mode === 'part') {  // and whole islands / parts of those
      const info = isl.info();
      for (const g of [...set]) for (const h of (mode === 'island' ? info.lists[info.isl.id[g]] : connectedPart(tp, g))) set.add(h);
    }
    tris = [...set];
  }
  if (state.material.id && !state.material.isFlat && !cachedTextures(state.material.id)) { status('Material is still loading…', true); return; }
  const opts = { ...strokeOpts(true), tool: erase ? 'erase' : 'paint' };
  if (!engine.fillTriangles(opts, tris)) { status('Select a visible layer to fill', true); return; }
  state.uvDirty = true;
  updateUndoButtons();
  status(`${erase ? 'Erased' : 'Filled'} ${tris.length.toLocaleString()} triangle${tris.length === 1 ? '' : 's'}`);
}

// ---- color picker: color, roughness and metallic of the model under the cursor become the flat material
const raycaster = new THREE.Raycaster();
function pickAt(pt) {
  const v = cssSize();
  raycaster.setFromCamera(new THREE.Vector2((pt.x / v.w) * 2 - 1, -(pt.y / v.h) * 2 + 1), camera);
  const hit = raycaster.intersectObject(mesh, false)[0];
  if (!hit || !hit.uv) { status('Nothing to pick there', true); return; }
  const s = engine.sampleAt(hit.uv.x, hit.uv.y);
  const hex = '#' + s.color.map((c) => Math.round(c * 255).toString(16).padStart(2, '0')).join('');
  selectMaterial('');
  state.material.flat.color = hex;
  state.material.flat.roughness = s.roughness;
  state.material.flat.metallic = s.metallic;
  $('fColor').value = hex;
  $('fRough').value = s.roughness; $('fRough').dispatchEvent(new Event('input'));
  $('fMetal').value = s.metallic; $('fMetal').dispatchEvent(new Event('input'));
  if (state.tool === 'pick' && state.pickReturn) setTool(state.pickReturn);
  status(`Picked ${hex}, roughness ${s.roughness.toFixed(2)}, metallic ${s.metallic.toFixed(2)}`);
}

function moveCursor(x, y, size) {
  const c = $('cursor');
  const vp = $('viewport').getBoundingClientRect();
  c.style.display = 'block';
  c.style.width = c.style.height = size + 'px';
  c.style.left = (x - vp.left - size / 2) + 'px';
  c.style.top = (y - vp.top - size / 2) + 'px';
  const t = state.quickPick ? 'pick' : state.tool;
  if (t === 'pick' || t === 'fill') { c.style.width = c.style.height = '14px'; c.style.left = (x - vp.left - 7) + 'px'; c.style.top = (y - vp.top - 7) + 'px'; }
  c.className = (t === 'erase' ? 'erase ' : t === 'island' ? 'island ' : t === 'blur' ? 'blur ' : t === 'pick' ? 'pick ' : t === 'fill' ? 'fill ' : '') +
    (state.brush.tip === 1 && ['paint', 'erase', 'blur'].includes(t) ? 'square' : '');
}

window.addEventListener('keydown', (e) => {
  if (e.target.tagName === 'INPUT' && e.target.type !== 'range' && e.target.type !== 'checkbox') return;
  if (e.key === 'Alt') { controls.mouseButtons.LEFT = THREE.MOUSE.ROTATE; e.preventDefault(); }
  const k = e.key.toLowerCase();
  if ((e.ctrlKey || e.metaKey) && k === 'z') { e.preventDefault(); e.shiftKey ? redo() : undo(); return; }
  if ((e.ctrlKey || e.metaKey) && k === 'y') { e.preventDefault(); redo(); return; }
  if ((e.ctrlKey || e.metaKey) && k === 's') { e.preventDefault(); $('btnSave').click(); return; }
  if ((e.ctrlKey || e.metaKey) && k === 'o') { e.preventDefault(); $('fileInput').click(); return; }
  if (isl.onKey(e, uvHovered)) return;
  if (e.ctrlKey || e.metaKey) return;
  if (k === 'l') setTool('island');
  else if (k === 'b') setTool('paint');
  else if (k === 'e') setTool('erase');
  else if (k === 'r') setTool('blur');
  else if (k === 'p') setTool('fill');
  else if (k === 'i' && !e.repeat) { state.quickPick = true; $('cursor').className = 'pick'; }
  else if (k === 'u') toggleUV();
  else if (k === 'f') frame();
  else if (e.key === '[') setBrushSize(state.brush.size / 1.15);
  else if (e.key === ']') setBrushSize(state.brush.size * 1.15);
  else if (e.key === 'Escape' && state.painting) { engine.cancelStroke(); state.painting = null; }
});
window.addEventListener('keyup', (e) => {
  if (e.key === 'Alt') controls.mouseButtons.LEFT = -1;
  if (e.key.toLowerCase() === 'i' && state.quickPick) {
    state.quickPick = false;
    // a tap without a pick selects the Pick tool (returning to the brush after one pick)
    if (!state.pickedDuringHold && state.tool !== 'pick') { state.pickReturn = state.tool; setTool('pick'); }
    state.pickedDuringHold = false;
  }
});
window.addEventListener('blur', () => { controls.mouseButtons.LEFT = -1; });

function undo() { if (engine.undo()) { refreshLayers(); state.uvDirty = true; } updateUndoButtons(); }
function redo() { if (engine.redo()) { refreshLayers(); state.uvDirty = true; } updateUndoButtons(); }
function updateUndoButtons() {
  $('btnUndo').disabled = !engine.undoStack.length;
  $('btnRedo').disabled = !engine.redoStack.length;
}

// ------------------------------------------------------------------ UV / texture view
let uvWire = null;
let uvHovered = false;
let uvLastDraw = 0;
const uvCanvas = $('uvCanvas');

function toggleUV() {
  state.uvVisible = !state.uvVisible;
  $('uvPanel').hidden = !state.uvVisible;
  $('btnUV').classList.toggle('active', state.uvVisible);
  state.uvDirty = true;
}

function uvBrushTexels() {
  const r = uvCanvas.getBoundingClientRect();
  return state.brush.size * engine.size / r.width;
}

function buildUVWire() {
  const N = uvCanvas.width;
  const c = document.createElement('canvas');
  c.width = c.height = N;
  const g = c.getContext('2d');
  g.strokeStyle = 'rgba(80, 200, 255, 0.55)';
  g.lineWidth = 0.6;
  const uv = mesh.geometry.attributes.uv.array;
  const tris = uv.length / 6;
  const stride = tris > 60000 ? Math.ceil(tris / 60000) : 1;  // keep huge meshes responsive
  g.beginPath();
  for (let t = 0; t < tris; t += stride) {
    const o = t * 6;
    g.moveTo(uv[o] * N, (1 - uv[o + 1]) * N);
    g.lineTo(uv[o + 2] * N, (1 - uv[o + 3]) * N);
    g.lineTo(uv[o + 4] * N, (1 - uv[o + 5]) * N);
    g.closePath();
  }
  g.stroke();
  uvWire = c;
}

function drawUV(force) {
  if (!state.uvVisible || (!state.uvDirty && !force)) return;
  const now = performance.now();
  if (!force && state.painting && now - uvLastDraw < 200) return;
  uvLastDraw = now;
  state.uvDirty = false;
  const m = engine.readMap(state.uvMap);
  const S = m.width, N = uvCanvas.width;
  const img = new ImageData(S, S);
  for (let y = 0; y < S; y++) img.data.set(m.data.subarray((S - 1 - y) * S * 4, (S - y) * S * 4), y * S * 4);
  const tmp = document.createElement('canvas');
  tmp.width = tmp.height = S;
  tmp.getContext('2d').putImageData(img, 0, 0);
  const g = uvCanvas.getContext('2d');
  g.imageSmoothingEnabled = true;
  g.clearRect(0, 0, N, N);
  g.drawImage(tmp, 0, 0, N, N);
  if ($('chkWire').checked) {
    if (!uvWire) buildUVWire();
    g.drawImage(uvWire, 0, 0);
  }
  isl.drawOverlay(g, N);
}

function uvPoint(e) {
  const r = uvCanvas.getBoundingClientRect();
  const u = (e.clientX - r.left) / r.width, v = 1 - (e.clientY - r.top) / r.height;
  return { x: u * engine.size, y: v * engine.size };
}
uvCanvas.addEventListener('pointerdown', (e) => {
  if (e.button !== 0) return;
  if (startStroke(uvPoint(e), true, e)) uvCanvas.setPointerCapture(e.pointerId);
});
uvCanvas.addEventListener('pointermove', (e) => {
  moveCursor(e.clientX, e.clientY, state.brush.size);
  if (state.painting && state.painting.uvSpace) moveStroke(uvPoint(e), e);
});
for (const ev of ['pointerup', 'pointercancel']) uvCanvas.addEventListener(ev, () => endStroke());
uvCanvas.addEventListener('pointerleave', () => { $('cursor').style.display = 'none'; uvHovered = false; });
uvCanvas.addEventListener('pointerenter', () => { uvHovered = true; });
$('selUVMap').addEventListener('change', (e) => { state.uvMap = e.target.value; drawUV(true); });
$('chkWire').addEventListener('change', () => drawUV(true));

// ------------------------------------------------------------------ UI: tools & brush
function setTool(t) {
  if (t !== 'pick') state.pickReturn = null;
  state.tool = t;
  $('fillModeRow').hidden = t !== 'fill';
  $('blurRow').hidden = t !== 'blur';
  for (const b of document.querySelectorAll('.tool[data-tool]')) b.classList.toggle('active', b.dataset.tool === t);
  $('islandBox').hidden = t !== 'island';
  selShow.value = t === 'island' || (state.island && state.island.uvMode === 'islands') ? 1 : 0;
  state.uvDirty = true;
  if (t === 'island') status('Island brush: paint over a part of the model, then press Enter (or Make island). Ctrl removes.');
  if (t === 'fill') status('Fill: click the model to fill a ' + ({ tri: 'triangle', island: 'UV island', part: 'connected part', all: 'whole model' })[state.brush.fillMode] + ' with the material; Ctrl+click erases');
  if (t === 'blur') status('Blur: paint over the layer to soften it (radius below)');
  if (t === 'pick') status('Pick: click the model to take its color, roughness and metallic');
}
for (const b of document.querySelectorAll('.tool[data-tool]')) b.addEventListener('click', () => setTool(b.dataset.tool));
$('btnUV').addEventListener('click', toggleUV);
$('btnFrame').addEventListener('click', frame);

function bindRange(id, get, set, fmt = (v) => v.toFixed(2)) {
  const el = $(id), out = $(id + 'V');
  el.value = get();
  const show = () => { if (out) out.textContent = fmt(parseFloat(el.value)); };
  el.addEventListener('input', () => { set(parseFloat(el.value)); show(); });
  show();
  return { el, refresh: () => { el.value = get(); show(); } };
}
const sizeCtl = bindRange('brSize', () => state.brush.size, (v) => { state.brush.size = v; }, (v) => v.toFixed(0));
function setBrushSize(v) {
  state.brush.size = Math.min(400, Math.max(2, v));
  sizeCtl.refresh();
}
bindRange('brHard', () => state.brush.hardness, (v) => { state.brush.hardness = v; });
bindRange('brOpacity', () => state.brush.opacity, (v) => { state.brush.opacity = v; });
bindRange('brFlow', () => state.brush.flow, (v) => { state.brush.flow = v; });
bindRange('brSpacing', () => state.brush.spacing, (v) => { state.brush.spacing = v; });
$('brTip').addEventListener('change', (e) => { state.brush.tip = parseInt(e.target.value, 10); });
for (const [id, k] of [['brPressSize', 'pressSize'], ['brPressFlow', 'pressFlow'], ['brThrough', 'through']])
  $(id).addEventListener('change', (e) => { state.brush[k] = e.target.checked; });
for (const [id, k] of [['chColor', 'color'], ['chRM', 'rm'], ['chHeight', 'height']])
  $(id).addEventListener('change', (e) => { state.channels[k] = e.target.checked; });
for (const [id, bit] of [['brSymX', 1], ['brSymY', 2], ['brSymZ', 4]])
  $(id).addEventListener('change', (e) => { state.brush.sym = e.target.checked ? state.brush.sym | bit : state.brush.sym & ~bit; });
$('fillMode').addEventListener('change', (e) => { state.brush.fillMode = e.target.value; });
bindRange('brBlur', () => state.brush.blurRadius, (v) => { state.brush.blurRadius = v; }, (v) => v.toFixed(1));

// material settings
bindRange('mScale', () => state.material.scale, (v) => { state.material.scale = v; });
$('mMapping').addEventListener('change', (e) => { state.material.mapping = parseInt(e.target.value, 10); });
$('mTint').addEventListener('input', (e) => { state.material.tint = e.target.value; });
bindRange('mHeight', () => state.material.heightAmt, (v) => { state.material.heightAmt = v; });
bindRange('mRough', () => state.material.roughOffset, (v) => { state.material.roughOffset = v; });
$('fColor').addEventListener('input', (e) => { state.material.flat.color = e.target.value; });
bindRange('fRough', () => state.material.flat.roughness, (v) => { state.material.flat.roughness = v; });
bindRange('fMetal', () => state.material.flat.metallic, (v) => { state.material.flat.metallic = v; });

// ------------------------------------------------------------------ library
const FLAT_THUMB = (() => {
  const c = document.createElement('canvas');
  c.width = c.height = 96;
  const g = c.getContext('2d');
  const grd = g.createRadialGradient(36, 34, 6, 48, 48, 46);
  grd.addColorStop(0, '#ffffff'); grd.addColorStop(0.25, '#c9c9c9'); grd.addColorStop(1, '#2a2a2a');
  g.fillStyle = grd;
  g.beginPath(); g.arc(48, 48, 44, 0, Math.PI * 2); g.fill();
  g.fillStyle = '#222'; g.font = 'bold 15px sans-serif'; g.textAlign = 'center'; g.fillText('flat', 48, 54);
  return c.toDataURL();
})();

function selectMaterial(id) {
  const m = state.material;
  if (!id) {
    m.id = ''; m.isFlat = true; m.defaultScale = 1;
    $('curThumb').src = FLAT_THUMB; $('curName').textContent = 'Flat color'; $('curDesc').textContent = 'paints the color / roughness / metallic below';
  } else {
    const e = findMaterial(id);
    if (!e) return;
    m.id = id;
    m.isFlat = e.type === 'flat';  // image materials sample their texture like library ones
    m.defaultScale = e.scale || 1;
    if (m.isFlat) {
      m.flat.color = e.flat.color; m.flat.roughness = e.flat.roughness; m.flat.metallic = e.flat.metallic;
      $('fColor').value = e.flat.color;
      $('fRough').value = e.flat.roughness; $('fMetal').value = e.flat.metallic;
      $('fRough').dispatchEvent(new Event('input')); $('fMetal').dispatchEvent(new Event('input'));
    }
    $('curThumb').src = e.thumb || FLAT_THUMB;
    $('curName').textContent = e.name;
    $('curDesc').textContent = e.description || e.category;
    if (!m.isFlat && e.type !== 'image') {
      status('Loading ' + e.name + '…');
      materialTextures(id, renderer.capabilities.getMaxAnisotropy()).then(() => status(e.name + ' ready'))
        .catch((err) => status('Could not load ' + e.name + ': ' + err.message, true));
    }
  }
  $('flatBox').hidden = !m.isFlat;
  for (const el of document.querySelectorAll('.mat')) el.classList.toggle('sel', el.dataset.id === m.id);
}

function buildLibrary() {
  const mats = libraryMaterials();
  $('libCount').textContent = mats.length ? `(${mats.length})` : '(not built)';
  const cats = ['All', ...new Set(mats.map((m) => m.category))];
  $('libCat').innerHTML = cats.map((c) => `<option>${c}</option>`).join('');
  const render = () => {
    const q = $('libSearch').value.toLowerCase();
    const cat = $('libCat').value;
    const grid = $('libGrid');
    grid.innerHTML = '';
    const add = (id, name, thumb, tip, badge) => {
      const d = document.createElement('div');
      d.className = 'mat' + (state.material.id === id ? ' sel' : '');
      d.dataset.id = id;
      d.title = tip;
      d.innerHTML = `<img src="${thumb}" alt="" loading="lazy">${badge ? '<b class="badge">P</b>' : ''}<span>${name}</span>`;
      d.addEventListener('click', () => selectMaterial(id));
      grid.appendChild(d);
    };
    if (!q && (cat === 'All' || !cat)) add('', 'Flat color', FLAT_THUMB, 'Paint plain color / roughness / metallic values', false);
    for (const m of mats) {
      if (cat && cat !== 'All' && m.category !== cat) continue;
      const hay = (m.name + ' ' + m.category + ' ' + m.id + ' ' + (m.tags || []).join(' ') + ' ' + (m.description || '')).toLowerCase();
      if (q && !hay.includes(q)) continue;
      add(m.id, m.name, m.thumb || FLAT_THUMB, `${m.name}\n${m.category}${m.type === 'image' ? '' : ' · Patina default library'}\n${m.description || ''}\n${m.id}`, m.type !== 'image');
    }
  };
  $('libSearch').addEventListener('input', render);
  $('libCat').addEventListener('change', render);
  render();
  rebuildLibrary = () => {
    const all = libraryMaterials();
    $('libCount').textContent = `(${all.length})`;
    const cur = $('libCat').value;
    const cs = ['All', ...new Set(all.map((m) => m.category))];
    $('libCat').innerHTML = cs.map((c) => `<option>${c}</option>`).join('');
    $('libCat').value = cs.includes(cur) ? cur : 'All';
    render();
  };
}
let rebuildLibrary = () => {};

// ------------------------------------------------------------------ layers
function refreshLayers() {
  const ul = $('layerList');
  ul.innerHTML = '';
  for (let i = engine.layers.length - 1; i >= 0; i--) {
    const l = engine.layers[i];
    const li = document.createElement('li');
    li.className = i === engine.selected ? 'sel' : '';
    const matName = l.kind === 'fill' ? (l.material.id ? (findMaterial(l.material.id) || {}).name || l.material.id : 'flat color') : '';
    li.innerHTML = `<input type="checkbox" ${l.visible ? 'checked' : ''} title="Visible"><span class="kind">${({ fill: 'FILL', adjust: 'ADJ' })[l.kind] || 'PAINT'}</span>` +
      `<span class="nm" title="Double-click to rename">${l.name}${matName ? ' <span class="dim">· ' + matName + '</span>' : ''}</span>`;
    li.querySelector('input').addEventListener('click', (e) => { e.stopPropagation(); l.visible = e.target.checked; engine.dirty = true; state.uvDirty = true; });
    li.addEventListener('click', () => { engine.selected = i; refreshLayers(); });
    li.addEventListener('dblclick', () => {
      const nm = li.querySelector('.nm');
      const inp = document.createElement('input');
      inp.value = l.name;
      inp.style.flex = '1';
      inp.style.minWidth = '0';
      nm.replaceWith(inp);
      inp.focus();
      inp.select();
      const done = (ok) => { if (ok && inp.value.trim()) l.name = inp.value.trim(); refreshLayers(); };
      inp.addEventListener('keydown', (e) => { if (e.key === 'Enter') done(true); if (e.key === 'Escape') done(false); e.stopPropagation(); });
      inp.addEventListener('blur', () => done(true));
    });
    ul.appendChild(li);
  }
  const base = document.createElement('li');
  base.className = 'base';
  base.innerHTML = '<span class="kind">BASE</span><span class="nm">Base material (below all layers)</span>';
  ul.appendChild(base);
  const l = engine.layers[engine.selected];
  $('lyOpacity').value = l ? l.opacity : 1;
  $('lyOpacityV').textContent = (l ? l.opacity : 1).toFixed(2);
  $('fillOpts').hidden = !(l && l.kind === 'fill');
  $('lyBlend').value = (l && l.blend) || 'normal';
  $('lyBlend').disabled = !l || l.kind === 'adjust';
  $('genOpts').hidden = !(l && l.kind !== 'paint');
  if (l && l.kind !== 'paint') {
    const g = l.gen || { type: 'none' };
    $('genType').value = g.type || 'none';
    $('genParams').hidden = !g.type || g.type === 'none';
    for (const [id, k, d] of [['genLevel', 'level', 0.5], ['genContrast', 'contrast', 0.1], ['genNoise', 'noise', 0.3]]) {
      $(id).value = g[k] ?? d;
      $(id + 'V').textContent = (+(g[k] ?? d)).toFixed(2);
    }
    $('genInvert').checked = !!g.invert;
  }
  $('adjOpts').hidden = !(l && l.kind === 'adjust');
  if (l && l.kind === 'adjust')
    for (const [id, k] of ADJ_KEYS) { $(id).value = l.adjust[k]; $(id + 'V').textContent = (+l.adjust[k]).toFixed(2); }
  if (l && l.kind === 'fill') {
    $('fchColor').checked = l.channels.color; $('fchRM').checked = l.channels.rm; $('fchHeight').checked = l.channels.height;
  }
  updateUndoButtons();
}

function currentMaterialCopy() {
  return { ...state.material, flat: { ...state.material.flat } };
}

$('lyAddPaint').addEventListener('click', () => { engine.addPaintLayer('Layer ' + (engine.layers.length + 1)); refreshLayers(); });
$('lyAddFill').addEventListener('click', async () => {
  const m = currentMaterialCopy();
  if (m.id && !m.isFlat) await materialTextures(m.id, renderer.capabilities.getMaxAnisotropy());
  const e = m.id ? findMaterial(m.id) : null;
  engine.addFillLayer(m, e ? e.name : 'Fill color');
  refreshLayers();
  status('Fill layer added: paint to shape its mask, erase to hide it ("Clear" hides it everywhere)');
});
$('lyAddAdj').addEventListener('click', () => {
  engine.addAdjustLayer('Adjustment');
  refreshLayers();
  status('Adjustment layer added: change hue, saturation, brightness, contrast, roughness or metallic below; paint or a smart mask shapes where');
});
$('lyBlend').addEventListener('change', (e) => {
  const l = engine.layers[engine.selected];
  if (l) { l.blend = e.target.value; engine.dirty = true; state.uvDirty = true; }
});
const curLayer = () => engine.layers[engine.selected];
const genOf = (l) => (l.gen ||= { type: 'none', level: 0.5, contrast: 0.1, noise: 0.3, invert: false });
$('genType').addEventListener('change', (e) => {
  const l = curLayer();
  if (!l || l.kind === 'paint') return;
  genOf(l).type = e.target.value;
  engine.dirty = true; state.uvDirty = true;
  refreshLayers();
});
for (const [id, k] of [['genLevel', 'level'], ['genContrast', 'contrast'], ['genNoise', 'noise']])
  $(id).addEventListener('input', (e) => {
    const l = curLayer();
    if (!l || l.kind === 'paint') return;
    genOf(l)[k] = parseFloat(e.target.value);
    $(id + 'V').textContent = parseFloat(e.target.value).toFixed(2);
    engine.dirty = true; state.uvDirty = true;
  });
$('genInvert').addEventListener('change', (e) => { const l = curLayer(); if (l && l.kind !== 'paint') { genOf(l).invert = e.target.checked; engine.dirty = true; state.uvDirty = true; } });
const ADJ_KEYS = [['adjHue', 'hue'], ['adjSat', 'saturation'], ['adjBright', 'brightness'], ['adjContrast', 'contrast'], ['adjRough', 'roughness'], ['adjMetal', 'metallic']];
for (const [id, k] of ADJ_KEYS)
  $(id).addEventListener('input', (e) => {
    const l = curLayer();
    if (!l || l.kind !== 'adjust') return;
    l.adjust[k] = parseFloat(e.target.value);
    $(id + 'V').textContent = l.adjust[k].toFixed(2);
    engine.dirty = true; state.uvDirty = true;
  });
$('lyDel').addEventListener('click', () => { engine.removeLayer(engine.selected); refreshLayers(); });
$('lyUp').addEventListener('click', () => { engine.moveLayer(engine.selected, engine.selected + 1); refreshLayers(); });
$('lyDown').addEventListener('click', () => { engine.moveLayer(engine.selected, engine.selected - 1); refreshLayers(); });
$('lyClear').addEventListener('click', () => { engine.clearLayer(engine.selected); refreshLayers(); state.uvDirty = true; });
$('lyOpacity').addEventListener('input', (e) => {
  const l = engine.layers[engine.selected];
  if (!l) return;
  l.opacity = parseFloat(e.target.value);
  $('lyOpacityV').textContent = l.opacity.toFixed(2);
  engine.dirty = true;
  state.uvDirty = true;
});
for (const [id, k] of [['fchColor', 'color'], ['fchRM', 'rm'], ['fchHeight', 'height']])
  $(id).addEventListener('change', (e) => {
    const l = engine.layers[engine.selected];
    if (l && l.kind === 'fill') { l.channels[k] = e.target.checked; engine.dirty = true; }
  });
$('fillUseMat').addEventListener('click', async () => {
  const l = engine.layers[engine.selected];
  if (!l || l.kind !== 'fill') return;
  l.material = currentMaterialCopy();
  if (l.material.id && !l.material.isFlat) await materialTextures(l.material.id, renderer.capabilities.getMaxAnisotropy());
  engine.dirty = true;
  refreshLayers();
});
$('fillShowAll').addEventListener('click', () => { engine.setFillMask(engine.selected, 1); state.uvDirty = true; });
$('baseColor').addEventListener('input', (e) => { engine.base.color = e.target.value; engine.dirty = true; state.uvDirty = true; });
$('baseRough').addEventListener('input', (e) => { engine.base.roughness = parseFloat(e.target.value); engine.dirty = true; });
bindRange('nStrength', () => engine.normalStrength, (v) => { engine.normalStrength = v; engine.dirty = true; });

// ------------------------------------------------------------------ top bar
$('btnOpen').addEventListener('click', () => $('fileInput').click());
$('btnSave').addEventListener('click', () => saveProject().catch((e) => status('Save failed: ' + e.message, true)));
$('libAddImage').addEventListener('click', () => $('fileInput').click());
$('fileInput').addEventListener('change', (e) => { if (e.target.files[0]) loadFile(e.target.files[0]); e.target.value = ''; });
$('selPrimitive').addEventListener('change', (e) => { if (e.target.value) primitive(e.target.value); e.target.value = ''; });
$('btnUndo').addEventListener('click', undo);
$('btnRedo').addEventListener('click', redo);
$('selRes').addEventListener('change', (e) => {
  engine.allocate(parseInt(e.target.value, 10), true);
  engine.clearUndo();
  applyMaps();
  refreshLayers();
  state.uvDirty = true;
  status('Texture resolution ' + e.target.value + ' (layers resampled; undo history cleared)');
});
// two-step button instead of confirm() (dialogs are unavailable in some embedded viewers)
let unwrapArmed = 0;
$('btnUnwrap').addEventListener('click', () => {
  const b = $('btnUnwrap');
  if (Date.now() - unwrapArmed > 4000) {
    unwrapArmed = Date.now();
    b.textContent = 'Click again to replace UVs';
    status('Unique UVs replaces the model UVs with a per-triangle layout (paint is transferred, undo history is cleared). Click again to go ahead.');
    setTimeout(() => { if (Date.now() - unwrapArmed >= 4000) b.textContent = 'Unique UVs'; }, 4100);
    return;
  }
  unwrapArmed = 0;
  b.textContent = 'Unique UVs';
  uniqueUVs();
});
$('btnHelp').addEventListener('click', () => { $('help').hidden = false; });
$('helpClose').addEventListener('click', () => { $('help').hidden = true; });
$('help').addEventListener('click', (e) => { if (e.target.id === 'help') $('help').hidden = true; });

// transfer paint to new UVs: rasterize in the new layout, sample the old textures at the old UVs
function uniqueUVs() {
  const g = mesh.geometry;
  const oldUV = g.attributes.uv.array.slice();
  atlasUnwrap(g, engine.size);
  isl.clearForNewUVs();  // pins and mirrored halves belong to the old layout
  g.setAttribute('uvOld', new THREE.BufferAttribute(oldUV, 2));
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
  for (const l of engine.layers)
    for (const k of ['color', 'rmh', 'mask']) {
      if (!l[k]) continue;
      const dst = new THREE.WebGLRenderTarget(engine.size, engine.size, { depthBuffer: false });
      engine._clear(dst, k === 'rmh' ? [0.5, 0, 0.5, 0] : [0, 0, 0, 0]);  // neutral height outside the islands
      mat.uniforms.uSrc.value = l[k].texture;
      renderer.setRenderTarget(dst);
      const ac = renderer.autoClear;
      renderer.autoClear = false;
      renderer.render(sc, engine.quadCam);
      renderer.autoClear = ac;
      l[k].dispose();
      l[k] = dst;
    }
  renderer.setRenderTarget(prev);
  mat.dispose();
  g.deleteAttribute('uvOld');
  computeTangents(g);
  engine.rasterUV();
  engine.clearUndo();
  engine.dirty = true;
  applyMaps();
  state.model.report = uvReport(g);
  uvWire = null;
  state.uvDirty = true;
  updateUndoButtons();
  status('Unique UVs generated - paint transferred. Exports use the new UVs.');
}

// ------------------------------------------------------------------ export
function mapToPngBytes(which, flip, directX = false) {
  const m = engine.readMap(which);
  const S = m.width;
  const c = document.createElement('canvas');
  c.width = c.height = S;
  const img = new ImageData(S, S);
  for (let y = 0; y < S; y++) {
    const src = flip ? S - 1 - y : y;
    img.data.set(m.data.subarray(src * S * 4, (src + 1) * S * 4), y * S * 4);
  }
  // normal maps are written OpenGL-style (green = image up). Without the row flip, +v points down the
  // image (glTF convention), so green has to be inverted to keep that meaning.
  if (which === 'normal' && (!flip) !== directX) for (let i = 1; i < img.data.length; i += 4) img.data[i] = 255 - img.data[i];
  for (let i = 3; i < img.data.length; i += 4) img.data[i] = 255;  // opaque maps (gutters were dilated)
  c.getContext('2d').putImageData(img, 0, 0);
  return { canvas: c, bytes: new Promise((res) => c.toBlob(async (b) => res(new Uint8Array(await b.arrayBuffer())), 'image/png')) };
}

function download(blob, name) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  document.body.appendChild(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 2000);
}

// maps per target program: [map, file suffix, options]
const PRESETS = {
  gltf: { prefix: '', maps: [['color', 'basecolor'], ['roughness', 'roughness'], ['metallic', 'metallic'], ['normal', 'normal_opengl'], ['height', 'height'], ['orm', 'orm']],
    readme: 'glTF / Blender: basecolor sRGB; roughness, metallic, height linear; normal OpenGL (Y+); orm = R occlusion (white), G roughness, B metallic.' },
  unreal: { prefix: 'T_', maps: [['color', 'BaseColor'], ['normal_dx', 'Normal'], ['orm', 'ORM'], ['height', 'Height']],
    readme: 'Unreal: BaseColor sRGB; Normal DirectX (Y-, as Unreal expects); ORM = R occlusion, G roughness, B metallic: import it with sRGB off and use the channels.' },
  unity: { prefix: '', maps: [['color', 'Albedo'], ['metal_smooth', 'MetallicSmoothness'], ['normal', 'Normal'], ['height', 'Height']],
    readme: 'Unity (URP / HDRP Lit, metallic workflow): Albedo sRGB; MetallicSmoothness = R metallic, A smoothness (1 - roughness); Normal OpenGL (mark it as a Normal map); Height for the height map slot.' },
};

async function exportBytes(which, flip) {
  if (which === 'normal_dx') {  // DirectX: green flipped relative to the OpenGL map
    const m = mapToPngBytes('normal', flip, true);
    return m.bytes;
  }
  if (which === 'metal_smooth') {
    const met = engine.readMap('metallic'), rough = engine.readMap('roughness');
    const S = met.width, c = document.createElement('canvas');
    c.width = c.height = S;
    const img = new ImageData(S, S);
    for (let y = 0; y < S; y++) {
      const src = flip ? S - 1 - y : y;
      for (let x = 0; x < S; x++) {
        const i = (src * S + x) * 4, o = (y * S + x) * 4;
        img.data[o] = met.data[i]; img.data[o + 1] = 0; img.data[o + 2] = 0; img.data[o + 3] = 255 - rough.data[i];
      }
    }
    c.getContext('2d').putImageData(img, 0, 0);
    return new Promise((res) => c.toBlob(async (b) => res(new Uint8Array(await b.arrayBuffer())), 'image/png'));
  }
  return mapToPngBytes(which, flip).bytes;
}

$('btnExportZip').addEventListener('click', async () => {
  const preset = PRESETS[$('selPreset').value] || PRESETS.gltf;
  status('Exporting textures…');
  // image row 0 is the top: OBJ / three.js UVs have v up, glTF UVs have v down
  const flip = state.model.convention === 'gl';
  const base = (state.model.name || 'patina').replace(/[^\w.-]+/g, '_');
  const files = [];
  for (const [which, suffix] of preset.maps)
    files.push({ name: `${preset.prefix}${base}_${suffix}.png`, data: await exportBytes(which, flip) });
  files.push({ name: 'README.txt', data: new TextEncoder().encode(`Textures painted in Patina Web (${engine.size}x${engine.size}).\n${preset.readme}\n`) });
  download(makeZip(files), base + '_textures_' + $('selPreset').value + '.zip');
  status('Exported ' + (files.length - 1) + ' maps for ' + $('selPreset').selectedOptions[0].textContent.replace('for ', ''));
});

// ------------------------------------------------------------------ projects (.patinaweb: a zip)
// project.json (settings, layers, materials), mesh.bin (positions, normals, UVs, island flags as
// raw arrays, in Patina Web's normalized space), layers/<n>_<map>.png, materials/<id>.png
// RGBA PNG written directly (a canvas would premultiply alpha and round the color of faint paint)
async function pngOf(data, S) {
  const raw = new Uint8Array(S * (S * 4 + 1));
  for (let y = 0; y < S; y++) raw.set(data.subarray(y * S * 4, (y + 1) * S * 4), y * (S * 4 + 1) + 1);
  const chunk = (type, body) => {
    const out = new Uint8Array(12 + body.length), dv = new DataView(out.buffer);
    dv.setUint32(0, body.length);
    for (let i = 0; i < 4; i++) out[4 + i] = type.charCodeAt(i);
    out.set(body, 8);
    dv.setUint32(8 + body.length, crc32(out.subarray(4, 8 + body.length)) >>> 0);
    return out;
  };
  const ihdr = new Uint8Array(13), dv = new DataView(ihdr.buffer);
  dv.setUint32(0, S); dv.setUint32(4, S);
  ihdr.set([8, 6, 0, 0, 0], 8);  // 8-bit RGBA
  const parts = [new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlibSync(raw, { level: 6 })), chunk('IEND', new Uint8Array(0))];
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}

async function saveProject() {
  status('Saving project…');
  const g = mesh.geometry;
  const P = g.attributes.position.array, N = g.attributes.normal.array, U = g.attributes.uv.array;
  const flags = state.island.flags;
  const bin = new Uint8Array(P.byteLength + N.byteLength + U.byteLength + flags.byteLength);
  let o = 0;
  for (const a of [P, N, U, flags]) { bin.set(new Uint8Array(a.buffer, a.byteOffset, a.byteLength), o); o += a.byteLength; }
  const files = [{ name: 'mesh.bin', data: bin }];
  const layers = [];
  for (let i = 0; i < engine.layers.length; i++) {
    const l = engine.layers[i];
    const keys = l.kind === 'paint' ? ['color', 'rmh'] : ['mask'];
    for (const k of keys) files.push({ name: `layers/${i}_${k}.png`, data: await pngOf(engine.readLayer(l, k), engine.size) });
    layers.push({ kind: l.kind, name: l.name, visible: l.visible, opacity: l.opacity, blend: l.blend || 'normal', gen: l.gen || null,
      adjust: l.adjust || null, material: l.material || null, channels: l.channels || null, maps: keys });
  }
  const mats = [];
  for (const m of customMaterials()) {
    files.push({ name: `materials/${m.id}.png`, data: m.png });
    mats.push({ id: m.id, name: m.name, roughness: m.roughness, metallic: m.metallic, scale: m.scale });
  }
  const json = {
    format: 'patina-web-project', version: 1, name: state.model.name, convention: state.model.convention,
    restore: state.model.restore.elements, size: engine.size, triangles: P.length / 9,
    base: engine.base, normalStrength: engine.normalStrength, selected: engine.selected, layers, customMaterials: mats,
    material: state.material, atlas: !!g.userData.atlas,
  };
  files.unshift({ name: 'project.json', data: new TextEncoder().encode(JSON.stringify(json, null, 1)) });
  const name = (state.model.name || 'patina').replace(/[^\w.-]+/g, '_') + '.patinaweb';
  download(makeZip(files), name);
  status('Saved ' + name + ' (' + layers.length + ' layers)');
}

async function imageFromBytes(bytes) {
  return createImageBitmap(new Blob([bytes]), { imageOrientation: 'none', premultiplyAlpha: 'none', colorSpaceConversion: 'none' });
}

async function openProject(file) {
  status('Opening ' + file.name + '…');
  try {
    const z = unzipSync(new Uint8Array(await file.arrayBuffer()));
    const j = JSON.parse(new TextDecoder().decode(z['project.json']));
    if (j.format !== 'patina-web-project') throw new Error('not a Patina Web project');
    for (const m of j.customMaterials || []) {
      const png = z[`materials/${m.id}.png`];
      if (png) addCustomMaterial(m.name, await imageFromBytes(png), png, { id: m.id, roughness: m.roughness, metallic: m.metallic, scale: m.scale });
    }
    rebuildLibrary();
    // the mesh, exactly as saved (already normalized; no automatic UV fixes)
    const T = j.triangles, bin = z['mesh.bin'];
    const buf = bin.buffer.slice(bin.byteOffset, bin.byteOffset + bin.byteLength);
    const P = new Float32Array(buf, 0, 9 * T), N = new Float32Array(buf, 36 * T, 9 * T), U = new Float32Array(buf, 72 * T, 6 * T);
    const F = new Uint8Array(buf, 96 * T, T);
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.BufferAttribute(new Float32Array(P), 3));
    g.setAttribute('normal', new THREE.BufferAttribute(new Float32Array(N), 3));
    g.setAttribute('uv', new THREE.BufferAttribute(new Float32Array(U), 2));
    g.userData.atlas = !!j.atlas;
    g.computeBoundingSphere();
    $('selRes').value = String(j.size);
    state.model.name = j.name;
    state.model.convention = j.convention;
    state.model.restore = new THREE.Matrix4().fromArray(j.restore);
    computeTangents(g);
    g.setAttribute('aCurv', new THREE.BufferAttribute(Isl.vertexCurvature(new Isl.Topo(g)), 1));
    isl.onGeometry(g);
    mesh.geometry.dispose();
    mesh.geometry = g;
    isl.setFlags(F);
    engine.setMesh(g, j.size);
    state.model.report = uvReport(g);
    // the layers
    for (const l of engine.layers) engine._disposeLayer(l);
    engine.layers = [];
    for (let i = 0; i < j.layers.length; i++) {
      const d = j.layers[i];
      if (d.material && d.material.id && !d.material.isFlat && findMaterial(d.material.id) && findMaterial(d.material.id).type !== 'image')
        await materialTextures(d.material.id, renderer.capabilities.getMaxAnisotropy());
      const l = d.kind === 'paint' ? engine.addPaintLayer(d.name) : d.kind === 'adjust' ? engine.addAdjustLayer(d.name) : engine.addFillLayer(d.material, d.name);
      Object.assign(l, { visible: d.visible, opacity: d.opacity, blend: d.blend, gen: d.gen || undefined });
      if (d.adjust) l.adjust = d.adjust;
      if (d.channels) l.channels = d.channels;
      for (const k of d.maps) {
        const png = z[`layers/${i}_${k}.png`];
        if (png) engine.loadLayerImage(l, k, await imageFromBytes(png));
      }
    }
    engine.selected = Math.min(Math.max(j.selected ?? 0, 0), engine.layers.length - 1);
    engine.clearUndo();
    Object.assign(engine.base, j.base || {});
    engine.normalStrength = j.normalStrength ?? 1;
    $('baseColor').value = engine.base.color;
    engine.dirty = true;
    applyMaps();
    frame();
    refreshLayers();
    uvWire = null;
    state.uvDirty = true;
    status(`Opened ${file.name}: ${T.toLocaleString()} triangles, ${engine.layers.length} layers`);
  } catch (e) {
    console.error(e);
    status('Could not open ' + file.name + ': ' + e.message, true);
  }
}

async function imageMaterial(file) {
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const img = await imageFromBytes(bytes);
    const e = addCustomMaterial(file.name.replace(/\.[^.]+$/, ''), img, bytes);
    rebuildLibrary();
    selectMaterial(e.id);
    status(`Material "${e.name}" made from your image (category Custom); paint or fill with it`);
  } catch (err) {
    status('Could not read ' + file.name + ': ' + err.message, true);
  }
}

$('btnExportGlb').addEventListener('click', () => {
  status('Exporting GLB…');
  const tex = (which, srgb) => {
    const t = new THREE.CanvasTexture(mapToPngBytes(which, false).canvas);
    t.flipY = false;  // canvas rows are already in UV order (v = 0 at the top in glTF)
    t.colorSpace = srgb ? THREE.SRGBColorSpace : THREE.NoColorSpace;
    return t;
  };
  const orm = tex('orm', false);
  const material = new THREE.MeshStandardMaterial({
    map: tex('color', true), roughnessMap: orm, metalnessMap: orm, normalMap: tex('normal', false), roughness: 1, metalness: 1,
  });
  const g = mesh.geometry.clone();
  for (const a of ['aSel', 'aTri', 'aMir', 'aCurv']) g.deleteAttribute(a);
  g.applyMatrix4(state.model.restore);
  const out = new THREE.Mesh(g, material);
  out.name = state.model.name || 'patina';
  new GLTFExporter().parse(out, (glb) => {
    download(new Blob([glb], { type: 'model/gltf-binary' }), (out.name.replace(/[^\w.-]+/g, '_')) + '.glb');
    status('Exported ' + out.name + '.glb');
  }, (err) => status('GLB export failed: ' + err.message, true), { binary: true });
});

// drag & drop
let dragDepth = 0;
window.addEventListener('dragenter', (e) => { e.preventDefault(); dragDepth++; $('dropHint').hidden = false; });
window.addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; $('dropHint').hidden = true; } });
window.addEventListener('dragover', (e) => e.preventDefault());
window.addEventListener('drop', (e) => {
  e.preventDefault();
  dragDepth = 0;
  $('dropHint').hidden = true;
  const f = e.dataTransfer.files[0];
  if (f) loadFile(f);
});

// ------------------------------------------------------------------ main loop
function resize() {
  const r = canvas.getBoundingClientRect();
  const w = Math.max(1, Math.floor(r.width)), h = Math.max(1, Math.floor(r.height));
  if (canvas.width !== Math.floor(w * renderer.getPixelRatio()) || canvas.height !== Math.floor(h * renderer.getPixelRatio())) {
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
  }
}

function loop() {
  resize();
  flushDabs();
  gizmo.update();
  engine.composite();
  renderer.setRenderTarget(null);
  renderer.render(scene, camera);
  drawUV(false);
  const l = engine.layers[engine.selected];
  $('viewInfo').textContent = (l ? l.name + (l.kind !== 'paint' ? ' [mask]' : '') : '(no layer)') + '  |  ' +
    (state.material.id ? (findMaterial(state.material.id) || {}).name : 'flat color') + '  |  ' + state.tool;
  $('statusRight').textContent = `${engine.size}px · ${engine.layers.length} layer${engine.layers.length === 1 ? '' : 's'}`;
  requestAnimationFrame(loop);
}

// expose for automation / tests (e.g. Playwright)
window.patinaWeb = { engine, state, camera, controls, gizmo, primitive, loadSample, selectMaterial, setTool, frame, toggleUV, uvReport, islands: isl, mesh,
  saveProject, openFile: loadFile, pickAt: (x, y) => { const v = cssSize(); pickAt({ x: x * v.w, y: y * v.h }); },
  fillAt: (x, y, erase = false) => { const v = cssSize(); fillAt({ x: x * v.w, y: y * v.h }, erase); },
  islandStroke: (points, remove = false) => {   // Island brush over viewport points [[x, y], ...] (0..1)
    const v = cssSize();
    const pts = points.map(([x, y]) => ({ x: x * v.w, y: y * v.h }));
    const fake = { ctrlKey: remove, metaKey: false };
    isl.brushDown(pts[0], fake);
    for (const p of pts.slice(1)) isl.brushMove(p, fake);
    isl.brushUp();
  },
  paintStroke: (points) => {   // points: [[x, y], ...] normalized to the viewport
    const v = cssSize();
    const pts = points.map(([x, y]) => ({ x: x * v.w, y: y * v.h }));
    const fake = { pointerType: 'mouse', shiftKey: false };
    if (!startStroke(pts[0], false, fake)) return false;
    for (const p of pts.slice(1)) moveStroke(p, fake);
    endStroke();
    return true;
  } };

buildLibrary();
selectMaterial('');
primitive('knot');
refreshLayers();
requestAnimationFrame(loop);
status(libraryMaterials().length ? 'Ready - pick a material and paint (Alt+drag or middle mouse to orbit, F1/Help for keys)'
  : 'Ready (the material library was not built: run scripts/build_web.py)');
