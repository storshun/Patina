#!/usr/bin/env python3
"""Builds the offline HTML manual: docs/manual/src/*.html -> docs/manual/*.html

    python3 scripts/build_manual.py

Each source page starts with  <!-- title: Page title -->  and optionally
<!-- nav: Short name -->. The command reference page is generated from
docs/COMMANDS.md. Everything is static and works from file:// URLs (the search
index is a .js file, not fetched JSON).
"""
import html
import json
import os
import re
import shutil
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAN = os.path.join(ROOT, "docs", "manual")
SRC = os.path.join(MAN, "src")
VERSION = re.search(r"project\(Patina VERSION ([0-9.]+)", open(os.path.join(ROOT, "CMakeLists.txt")).read()).group(1)

TEMPLATE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} - Patina manual</title>
<link rel="icon" type="image/png" href="img/icon.png">
<link rel="stylesheet" href="manual.css">
</head>
<body>
<header class="top">
  <button class="menu-btn" aria-label="Menu" onclick="document.body.classList.toggle('nav-open')">☰</button>
  <a class="brand" href="index.html"><img class="logo" src="img/icon.png" alt=""> Patina <span class="ver">{version} manual</span></a>
  <input id="search" type="search" placeholder="Search the manual" autocomplete="off">
  <div id="results" hidden></div>
</header>
<nav class="side">{nav}</nav>
<main>
<article>
{body}
</article>
<footer class="pager">{pager}</footer>
</main>
<script src="search-index.js"></script>
<script src="manual.js"></script>
</body>
</html>
"""


def md_commands():
    """docs/COMMANDS.md -> HTML (marked if available, else a tiny converter)"""
    path = os.path.join(ROOT, "docs", "COMMANDS.md")
    md = open(path, encoding="utf-8").read()
    marked = shutil.which("marked")
    if marked:
        out = subprocess.run([marked, "--gfm"], input=md, capture_output=True, text=True)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout
    # fallback: headings, tables, code, paragraphs
    lines, res, table = md.splitlines(), [], None
    for ln in lines:
        if ln.startswith("|"):
            cells = [c.strip() for c in ln.strip("|").split("|")]
            if all(set(c) <= set("-: ") for c in cells):
                continue
            if table is None:
                table = []
                res.append("<table><thead><tr>" + "".join(f"<th>{inline(c)}</th>" for c in cells) + "</tr></thead><tbody>")
            else:
                res.append("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in cells) + "</tr>")
            continue
        if table is not None:
            res.append("</tbody></table>")
            table = None
        m = re.match(r"^(#+)\s+(.*)", ln)
        if m:
            n = len(m.group(1))
            res.append(f"<h{n}>{inline(m.group(2))}</h{n}>")
        elif ln.strip():
            res.append(f"<p>{inline(ln)}</p>")
    if table is not None:
        res.append("</tbody></table>")
    return "\n".join(res)


def md_changelog():
    """CHANGELOG.md -> HTML: headings, nested bullet lists, paragraphs (links to repository files as plain text)"""
    md = open(os.path.join(ROOT, "CHANGELOG.md"), encoding="utf-8").read()
    # blocks: ("h", level, text), ("li", depth, text), ("p", 0, text); a line that goes on joins the block before it
    blocks = []
    for ln in md.splitlines()[1:]:  # (the page has its own title)
        h = re.match(r"^(#+)\s+(.*)", ln)
        li = re.match(r"^(\s*)- (.*)", ln)
        if h:
            blocks.append(["h", len(h.group(1)), h.group(2)])
        elif li:
            blocks.append(["li", len(li.group(1)) // 2 + 1, li.group(2)])
        elif not ln.strip():
            blocks.append(["gap", 0, ""])
        elif blocks and blocks[-1][0] in ("li", "p"):
            blocks[-1][2] += " " + ln.strip()
        else:
            blocks.append(["p", 0, ln.strip()])

    def text(t):
        return inline(re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r"\1", t))  # (the links point into the repository)

    res, depth = [], 0
    for kind, n, t in blocks:
        if kind == "li":
            if n > depth:
                while depth < n:
                    res.append("<ul><li>")
                    depth += 1
            else:
                while depth > n:
                    res.append("</li></ul>")
                    depth -= 1
                res.append("</li><li>")
            res.append(text(t))
            continue
        if kind == "gap":
            continue
        while depth:
            res.append("</li></ul>")
            depth -= 1
        res.append(f"<h{n}>{text(t)}</h{n}>" if kind == "h" else f"<p>{text(t)}</p>")
    while depth:
        res.append("</li></ul>")
        depth -= 1
    return "\n".join(res)


def inline(s):
    s = html.escape(s, quote=False)
    s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
    s = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", s)
    s = re.sub(r"\*([^*]+)\*", r"<i>\1</i>", s)
    return s


ICONS = os.path.join(ROOT, "icons")


def icons(body):
    """[[icon:paint]] -> the toolbar icon (icons/paint.svg) inline, in the text's color with the copper accent"""
    def rep(m):
        with open(os.path.join(ICONS, m.group(1) + ".svg"), encoding="utf-8") as f:
            svg = f.read().strip()
        return svg.replace("<svg ", '<svg class="ic" aria-hidden="true" width="1.35em" height="1.35em" ', 1).replace("\n", "")
    return re.sub(r"\[\[icon:([a-z0-9_]+)\]\]", rep, body)


def slug(t):
    return re.sub(r"[^a-z0-9]+", "-", re.sub(r"<[^>]+>", "", t).lower()).strip("-")


def add_ids(body):
    """id attributes on h2/h3 (for the in-page TOC and search links)"""
    seen = set()

    def rep(m):
        tag, attrs, text = m.group(1), m.group(2) or "", m.group(3)
        if "id=" in attrs:
            return m.group(0)
        i = slug(text) or "s"
        while i in seen:
            i += "-2"
        seen.add(i)
        return f'<{tag}{attrs} id="{i}">{text}</{tag}>'

    return re.sub(r"<(h[23])(\s[^>]*)?>(.*?)</\1>", rep, body, flags=re.S)


def main():
    pages = []
    for f in sorted(os.listdir(SRC)):
        if not f.endswith(".html"):
            continue
        text = open(os.path.join(SRC, f), encoding="utf-8").read()
        title = re.search(r"<!--\s*title:\s*(.*?)\s*-->", text).group(1)
        navm = re.search(r"<!--\s*nav:\s*(.*?)\s*-->", text)
        name = re.sub(r"^\d+[a-z]?-", "", f)
        if "<!-- commands -->" in text:
            text = text.replace("<!-- commands -->", md_commands())
        if "<!-- changelog -->" in text:
            text = text.replace("<!-- changelog -->", md_changelog())
        pages.append({"file": name, "title": title, "nav": navm.group(1) if navm else title, "body": add_ids(icons(text))})
    index = []
    for i, p in enumerate(pages):
        nav = []
        for q in pages:
            cls = ' class="cur"' if q is p else ""
            nav.append(f'<a href="{q["file"]}"{cls}>{html.escape(q["nav"])}</a>')
            if q is p:
                subs = re.findall(r'<h2[^>]*id="([^"]+)"[^>]*>(.*?)</h2>', p["body"], flags=re.S)
                if subs:
                    nav.append('<div class="toc">' + "".join(
                        f'<a href="#{sid}">{re.sub(r"<[^>]+>", "", st)}</a>' for sid, st in subs) + "</div>")
        prev = pages[i - 1] if i > 0 else None
        nxt = pages[i + 1] if i + 1 < len(pages) else None
        pager = (f'<a class="prev" href="{prev["file"]}">← {html.escape(prev["nav"])}</a>' if prev else "<span></span>") + \
                (f'<a class="next" href="{nxt["file"]}">{html.escape(nxt["nav"])} →</a>' if nxt else "<span></span>")
        out = TEMPLATE.format(title=html.escape(p["title"]), version=VERSION, nav="\n".join(nav), body=p["body"],
                              pager=pager)
        with open(os.path.join(MAN, p["file"]), "w", encoding="utf-8") as fo:
            fo.write(out)
        # search index: one entry per h2/h3 section
        parts = re.split(r'(<h[23][^>]*id="[^"]+"[^>]*>.*?</h[23]>)', p["body"], flags=re.S)
        cur_id, cur_title = "", p["title"]
        for part in parts:
            m = re.match(r'<h[23][^>]*id="([^"]+)"[^>]*>(.*?)</h[23]>', part, flags=re.S)
            if m:
                cur_id, cur_title = m.group(1), re.sub(r"<[^>]+>", "", m.group(2))
                continue
            txt = re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", " ", part))).strip()
            if txt:
                index.append({"p": p["file"], "a": cur_id, "page": p["nav"], "t": cur_title, "x": txt[:1200]})
    with open(os.path.join(MAN, "search-index.js"), "w", encoding="utf-8") as f:
        f.write("window.PATINA_SEARCH = " + json.dumps(index, separators=(",", ":")) + ";\n")
    print(f"manual: {len(pages)} pages, {len(index)} search sections -> {MAN}")


if __name__ == "__main__":
    main()
