// Patina Web - the axis gizmo in the 3D view's top right corner, the same as the desktop app's
// (src/ui/ViewGizmo.cpp): the X, Y and Z axes turning with the camera (X red, Y green, Z blue; the negative
// ends as plain dots). A click on an end jumps to the view along that axis (on the one facing you: from the other
// side; 0.38: a jump, as the desktop app's numpad), a drag orbits, the wheel zooms.
import * as THREE from 'three';

const COLORS = [[240, 82, 95], [125, 194, 66], [74, 144, 240]];
const NAMES = ['X', '-X', 'Y', '-Y', 'Z', '-Z'];
const VIEWS = ['Right view', 'Left view', 'Top view', 'Bottom view', 'Front view', 'Back view'];
// the camera's angles round the target for each end (three.js spherical: theta from +Z toward +X, phi from +Y);
// the desktop's views: yaw = theta, pitch = 90 degrees - phi
const POLE = Math.PI / 2 - 1.5699;
const ANGLES = [[Math.PI / 2, Math.PI / 2], [-Math.PI / 2, Math.PI / 2], [0, POLE], [0, Math.PI - POLE],
  [0, Math.PI / 2], [Math.PI, Math.PI / 2]];
const SIZE = 88, R = 44, B = 9, REACH = R - B - 2, TURN = 0.24;
const animate = false;  // (0.38) views jump; true turns to them in TURN seconds

export function setupGizmo({ container, canvas, camera, controls, avoid }) {
  const el = document.createElement('canvas');
  el.id = 'axisGizmo';
  container.appendChild(el);
  const ctx = el.getContext('2d');
  const g = { hot: -1, over: false, pressed: false, dragged: false, at: null, turn: null, shown: true };
  const axisVec = (i) => { const v = new THREE.Vector3(); v.setComponent(i >> 1, i & 1 ? -1 : 1); return v; };
  const rot = new THREE.Matrix4();

  // where each end is (css pixels in the gizmo) and how far toward the viewer
  function ends() {
    camera.updateMatrixWorld();
    rot.extractRotation(camera.matrixWorldInverse);
    const out = [];
    for (let i = 0; i < 6; ++i) {
      const v = axisVec(i).applyMatrix4(rot);
      out.push({ id: i, x: R + v.x * REACH, y: R - v.y * REACH, depth: v.z });
    }
    return out;
  }
  function endAt(x, y) {
    let best = -1, depth = -2;
    for (const e of ends())
      if ((e.x - x) ** 2 + (e.y - y) ** 2 <= (B + 1) ** 2 && e.depth > depth) best = e.id, depth = e.depth;
    return best;
  }
  function facing() {
    const d = camera.position.clone().sub(controls.target).normalize();
    for (let i = 0; i < 6; ++i) if (d.dot(axisVec(i)) > 0.99996) return i;
    return -1;
  }
  function place(theta, phi) {
    const off = new THREE.Vector3().setFromSpherical(new THREE.Spherical(camera.position.distanceTo(controls.target), phi, theta));
    camera.position.copy(controls.target).add(off);
    camera.lookAt(controls.target);
    controls.update();
  }
  function lookAlong(i) {
    if (facing() === i) i ^= 1;
    const s = new THREE.Spherical().setFromVector3(camera.position.clone().sub(controls.target));
    let [theta, phi] = ANGLES[i];
    theta += Math.round((s.theta - theta) / (2 * Math.PI)) * 2 * Math.PI;  // the shorter way round
    if (!animate) { g.turn = null; place(theta, phi); return; }
    g.turn = { from: [s.theta, s.phi], to: [theta, phi], t0: performance.now(), last: null };
  }
  const local = (e) => { const r = el.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
  const inside = (x, y) => (x - R) ** 2 + (y - R) ** 2 <= R * R;

  el.addEventListener('pointerdown', (e) => {
    const [x, y] = local(e);
    if (e.button !== 0 || !inside(x, y)) return;
    e.preventDefault();
    el.setPointerCapture(e.pointerId);
    g.pressed = true, g.dragged = false, g.at = [e.clientX, e.clientY], g.turn = null;
  });
  el.addEventListener('pointermove', (e) => {
    const [x, y] = local(e);
    g.over = g.pressed || inside(x, y);
    if (g.pressed) {
      if (!g.dragged && (e.clientX - g.at[0]) ** 2 + (e.clientY - g.at[1]) ** 2 > 9) g.dragged = true;
      if (g.dragged) {
        // the same way round as the desktop's orbit (drag right: the camera goes left round the model)
        const s = new THREE.Spherical().setFromVector3(camera.position.clone().sub(controls.target));
        place(s.theta - e.movementX * 0.006, Math.min(Math.PI - 1e-4, Math.max(1e-4, s.phi - e.movementY * 0.006)));
        g.hot = -1;
        return;
      }
    }
    g.hot = g.over ? endAt(x, y) : -1;
    el.title = g.hot >= 0 ? VIEWS[facing() === g.hot ? g.hot ^ 1 : g.hot] + '. Click an axis to look along it, drag to orbit.' : '';
  });
  el.addEventListener('pointerup', (e) => {
    if (!g.pressed) return;
    const [x, y] = local(e);
    g.pressed = false;
    if (!g.dragged) {
      const i = endAt(x, y);
      if (i >= 0) lookAlong(i);
    }
  });
  el.addEventListener('pointerleave', () => { if (!g.pressed) g.over = false, g.hot = -1; });
  // the wheel zooms, as over the rest of the view
  el.addEventListener('wheel', (e) => {
    e.preventDefault();
    canvas.dispatchEvent(new WheelEvent('wheel', e));
  }, { passive: false });

  function draw() {
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    if (el.width !== Math.round(SIZE * dpr)) el.width = el.height = Math.round(SIZE * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, SIZE, SIZE);
    if (g.over) {
      ctx.fillStyle = `rgba(255,255,255,${g.pressed && g.dragged ? 0.16 : 0.1})`;
      ctx.beginPath(); ctx.arc(R, R, R, 0, 2 * Math.PI); ctx.fill();
    }
    ctx.font = '600 12px "Noto Sans", system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    for (const e of ends().sort((a, b) => a.depth - b.depth)) {
      const shade = 0.72 + 0.28 * Math.min(1, Math.max(0, e.depth * 0.5 + 0.5));
      const [r, gg, b] = COLORS[e.id >> 1].map((c) => Math.round(c * shade));
      const col = `rgb(${r},${gg},${b})`, hot = g.hot === e.id;
      ctx.fillStyle = 'rgba(0,0,0,0.35)';
      ctx.beginPath(); ctx.arc(e.x, e.y, B + 1, 0, 2 * Math.PI); ctx.fill();
      if (!(e.id & 1)) {
        const dx = e.x - R, dy = e.y - R, len = Math.hypot(dx, dy);
        if (len > B) {
          ctx.lineCap = 'round';
          for (const [w, s] of [[4, 'rgba(0,0,0,0.27)'], [2, col]]) {
            ctx.strokeStyle = s; ctx.lineWidth = w;
            ctx.beginPath(); ctx.moveTo(R, R); ctx.lineTo(e.x - dx / len * B, e.y - dy / len * B); ctx.stroke();
          }
        }
        ctx.fillStyle = col;
        ctx.beginPath(); ctx.arc(e.x, e.y, B, 0, 2 * Math.PI); ctx.fill();
        ctx.fillStyle = '#161618';
        ctx.fillText(NAMES[e.id], e.x, e.y + 0.5);
      } else {
        ctx.fillStyle = `rgba(${r},${gg},${b},0.42)`;
        ctx.beginPath(); ctx.arc(e.x, e.y, B, 0, 2 * Math.PI); ctx.fill();
        ctx.strokeStyle = col; ctx.lineWidth = 1.25;
        ctx.beginPath(); ctx.arc(e.x, e.y, B - 0.5, 0, 2 * Math.PI); ctx.stroke();
        if (hot) { ctx.fillStyle = '#fff'; ctx.font = '600 11px "Noto Sans", system-ui, sans-serif'; ctx.fillText(NAMES[e.id], e.x, e.y + 0.5);
          ctx.font = '600 12px "Noto Sans", system-ui, sans-serif'; }
      }
      if (hot) {
        ctx.strokeStyle = 'rgba(255,255,255,0.9)'; ctx.lineWidth = 1.5;
        ctx.beginPath(); ctx.arc(e.x, e.y, B + 0.5, 0, 2 * Math.PI); ctx.stroke();
      }
    }
  }

  // each frame, before the view is drawn: the turn to a view (it stops when anything else moves the camera),
  // the gizmo clear of the UV panel, the picture
  function update() {
    el.style.display = g.shown ? '' : 'none';
    if (!g.shown) return;
    const t = g.turn;
    if (t) {
      if (t.last && camera.position.distanceToSquared(t.last) > 1e-10) {
        g.turn = null;
      } else {
        const k = Math.min(1, (performance.now() - t.t0) / 1000 / TURN), s = k * k * (3 - 2 * k);
        place(t.from[0] + (t.to[0] - t.from[0]) * s, t.from[1] + (t.to[1] - t.from[1]) * s);
        t.last = camera.position.clone();
        if (k >= 1) g.turn = null;
      }
    }
    el.style.right = avoid && !avoid.hidden ? (avoid.offsetWidth + 20) + 'px' : '10px';
    draw();
  }

  return {
    update,
    lookAlong,
    // for tests: each end's place in the page (css pixels) and the view a click there goes to
    ends: () => { const r = el.getBoundingClientRect(); return ends().map((e) => ({ name: NAMES[e.id], x: r.left + e.x, y: r.top + e.y, depth: e.depth })); },
    get shown() { return g.shown; },
    set shown(v) { g.shown = !!v; },
    turning: () => !!g.turn,
  };
}
