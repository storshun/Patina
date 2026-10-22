// Offline search over the manual (index in search-index.js) and small niceties.
(function () {
  const input = document.getElementById('search');
  const box = document.getElementById('results');
  const index = window.PATINA_SEARCH || [];
  let active = -1;

  function esc(s) { return s.replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c])); }
  function mark(text, words) {
    let out = esc(text);
    for (const w of words) if (w.length > 1) out = out.replace(new RegExp('(' + w.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + ')', 'ig'), '<mark>$1</mark>');
    return out;
  }
  function snippet(x, words) {
    const lx = x.toLowerCase();
    let i = -1;
    for (const w of words) { i = lx.indexOf(w); if (i >= 0) break; }
    const s = Math.max(0, i - 60);
    return (s > 0 ? '…' : '') + x.slice(s, s + 170) + (x.length > s + 170 ? '…' : '');
  }
  function search(q) {
    const words = q.toLowerCase().split(/\s+/).filter(Boolean);
    if (!words.length) return [];
    const res = [];
    for (const e of index) {
      const t = e.t.toLowerCase(), x = e.x.toLowerCase();
      let score = 0, ok = true;
      for (const w of words) {
        const inT = t.includes(w), inX = x.includes(w);
        if (!inT && !inX) { ok = false; break; }
        score += (inT ? 10 : 0) + (inX ? 1 : 0);
      }
      if (ok) res.push({ e, score });
    }
    res.sort((a, b) => b.score - a.score);
    return res.slice(0, 12).map((r) => ({ ...r.e, words }));
  }
  function render() {
    const q = input.value.trim();
    const res = search(q);
    active = -1;
    if (!q) { box.hidden = true; return; }
    box.hidden = false;
    box.innerHTML = res.length ? res.map((r) => `<a href="${r.p}${r.a ? '#' + r.a : ''}"><div class="rp">${esc(r.page)}</div>` +
      `<div>${mark(r.t, r.words)}</div><div class="rx">${mark(snippet(r.x, r.words), r.words)}</div></a>`).join('')
      : '<div class="rx" style="padding:10px">No matches</div>';
  }
  if (input) {
    input.addEventListener('input', render);
    input.addEventListener('keydown', (e) => {
      const links = box.querySelectorAll('a');
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        e.preventDefault();
        active = Math.max(0, Math.min(links.length - 1, active + (e.key === 'ArrowDown' ? 1 : -1)));
        links.forEach((l, i) => l.classList.toggle('active', i === active));
      } else if (e.key === 'Enter' && links.length) {
        location.href = links[Math.max(0, active)].href;
      } else if (e.key === 'Escape') {
        input.value = ''; render();
      }
    });
    document.addEventListener('click', (e) => { if (!box.contains(e.target) && e.target !== input) box.hidden = true; });
    document.addEventListener('keydown', (e) => {
      if (e.key === '/' && document.activeElement !== input) { e.preventDefault(); input.focus(); }
    });
  }
  // close the mobile menu after choosing a link
  document.querySelectorAll('nav.side a').forEach((a) => a.addEventListener('click', () => document.body.classList.remove('nav-open')));
})();
