// Material library for Patina Web. The material list and thumbnails come from
// library/index.js; each material's maps load on demand from library/<id>.js
// (script tags + data: URIs, so everything also works from file:// URLs).
import * as THREE from 'three';

const loading = new Map();
const textures = new Map();

// materials made in this session from the user's images (category "Custom")
const custom = [];

export function libraryMaterials() {
  const L = window.PatinaLibrary;
  return [...custom, ...(L && L.materials ? L.materials : [])];
}

export function customMaterials() {
  return custom;
}

// A material from an image: its color tiles, with flat roughness / metallic / height.
// `image`: an HTMLImageElement or ImageBitmap; `png`: its bytes (kept for saving projects).
export function addCustomMaterial(name, image, png, opts = {}) {
  const id = opts.id || 'custom_' + (custom.length + 1) + '_' + name.replace(/[^\w]+/g, '_').toLowerCase().slice(0, 24);
  const thumb = document.createElement('canvas');
  thumb.width = thumb.height = 96;
  thumb.getContext('2d').drawImage(image, 0, 0, 96, 96);
  const rough = opts.roughness ?? 0.6, metal = opts.metallic ?? 0;
  const rmh = document.createElement('canvas');
  rmh.width = rmh.height = 4;
  const g = rmh.getContext('2d');
  g.fillStyle = `rgb(${Math.round(rough * 255)}, ${Math.round(metal * 255)}, 128)`;
  g.fillRect(0, 0, 4, 4);
  const entry = { id, name, category: 'Custom', description: 'Made from your image (' + name + ')', type: 'image',
    scale: opts.scale || 1, tags: ['custom', 'image'], thumb: thumb.toDataURL(), png, roughness: rough, metallic: metal };
  const old = custom.findIndex((m) => m.id === id);
  if (old >= 0) custom.splice(old, 1);
  custom.unshift(entry);
  textures.set(id, { color: makeTexture(image, 8), rmh: makeTexture(rmh, 1) });
  return entry;
}

export function findMaterial(id) {
  return libraryMaterials().find((m) => m.id === id) || null;
}

function loadScript(src) {
  return new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = src;
    s.onload = () => resolve();
    s.onerror = () => reject(new Error('could not load ' + src));
    document.head.appendChild(s);
  });
}

function loadImage(uri) {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = () => reject(new Error('bad image'));
    img.src = uri;
  });
}

function makeTexture(img, anisotropy) {
  const t = new THREE.Texture(img);
  t.wrapS = t.wrapT = THREE.RepeatWrapping;
  t.colorSpace = THREE.NoColorSpace; // painting works in sRGB values, like the desktop app
  t.generateMipmaps = true;
  t.minFilter = THREE.LinearMipmapLinearFilter;
  t.anisotropy = anisotropy;
  t.needsUpdate = true;
  return t;
}

// -> Promise<{color: Texture, rmh: Texture}> (flat materials resolve to null)
export function materialTextures(id, anisotropy = 8) {
  if (textures.has(id)) return Promise.resolve(textures.get(id));
  if (loading.has(id)) return loading.get(id);
  const m = findMaterial(id);
  if (!m || m.type === 'flat' || m.type === 'image') return Promise.resolve(null);
  const p = (async () => {
    const L = window.PatinaLibrary;
    if (!L.maps[id]) await loadScript('library/' + id + '.js');
    const maps = L.maps[id];
    const [c, r] = await Promise.all([loadImage(maps.color), loadImage(maps.rmh)]);
    const tex = { color: makeTexture(c, anisotropy), rmh: makeTexture(r, anisotropy) };
    textures.set(id, tex);
    loading.delete(id);
    return tex;
  })();
  loading.set(id, p);
  return p;
}

export function cachedTextures(id) {
  return textures.get(id) || null;
}

export function hexToRgb(hex) {
  const v = parseInt((hex || '#808080').slice(1), 16);
  return [((v >> 16) & 255) / 255, ((v >> 8) & 255) / 255, (v & 255) / 255];
}

export function rgbToHex(c) {
  const h = (x) => Math.round(Math.min(1, Math.max(0, x)) * 255).toString(16).padStart(2, '0');
  return '#' + h(c[0]) + h(c[1]) + h(c[2]);
}
