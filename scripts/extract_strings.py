#!/usr/bin/env python3
"""Makes languages/patina.pot, the template for translating Patina's interface, from the text the code marks
with T_ L_ Z_ P_ C_ F_ N_ (src/ui/Tr.h). Translators copy it to <code>.po and fill it in (Poedit, Weblate...).

  extract_strings.py [root]            write languages/patina.pot
  extract_strings.py --check [root]    exit 1 when the template is out of date (the ctest translation_template)
  extract_strings.py --stats [root]    how many texts, and the files with the most
"""
import os
import re
import sys

MACROS = {"T_": 1, "L_": 1, "Z_": 1, "N_": 1, "F_": 1, "P_": 2, "C_": 2}
SKIP = {os.path.join("src", "core", "I18n.h"), os.path.join("src", "core", "I18n.cpp"),
        os.path.join("src", "ui", "Tr.h"), os.path.join("src", "ui", "Tr.cpp")}
FORMAT = re.compile(r"%[-+ #0']*(\*|\d+)?(\.(\*|\d+))?(hh|h|ll|l|L|q|j|z|t)?[diouxXeEfFgGaAcsp]")


def tokens(text):
    """C++ tokens: (kind, value, line); kinds: id, str, punct. Comments and char literals are skipped."""
    i, n, line = 0, len(text), 1
    out = []
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in " \t\r\f\v":
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += text.count("\n", i, j)
            i = j
        elif c == "#" and (not out or out[-1][2] != line):  # a preprocessor line (#include, #define...)
            while i < n and text[i] != "\n":
                if text[i] == "\\" and i + 1 < n and text[i + 1] == "\n":
                    line += 1
                    i += 1
                i += 1
        elif c == "R" and text.startswith('R"', i) or text.startswith('u8R"', i):
            k = text.index('"', i)
            p = text.index("(", k)
            delim = ")" + text[k + 1:p] + '"'
            e = text.index(delim, p)
            out.append(("str", text[p + 1:e], line))
            line += text.count("\n", i, e)
            i = e + len(delim)
        elif c == '"' or (text.startswith('u8"', i) or (c in "LuU" and i + 1 < n and text[i + 1] == '"')):
            i = text.index('"', i) + 1
            j = i
            while text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(("str", unescape(text[i:j]), line))
            i = j + 1
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        elif c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            out.append(("id", text[i:j], line))
            i = j
        elif c.isdigit():
            j = i
            while j < n and (text[j].isalnum() or text[j] in "._'"):
                j += 1
            i = j
        else:
            if text.startswith("::", i) or text.startswith("->", i):
                out.append(("punct", text[i:i + 2], line))
                i += 2
            else:
                out.append(("punct", c, line))
                i += 1
    return out


def unescape(s):
    out, i = [], 0
    simple = {"n": "\n", "t": "\t", "r": "\r", "a": "\a", "b": "\b", "f": "\f", "v": "\v", '"': '"', "'": "'",
              "\\": "\\", "?": "?"}
    while i < len(s):
        c = s[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        e = s[i + 1]
        if e in simple:
            out.append(simple[e])
            i += 2
        elif e == "x":
            j = i + 2
            while j < len(s) and s[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(chr(int(s[i + 2:j], 16)))
            i = j
        elif e in "01234567":
            j = i + 1
            while j < len(s) and j < i + 4 and s[j] in "01234567":
                j += 1
            out.append(chr(int(s[i + 1:j], 8)))
            i = j
        else:
            out.append(e)
            i += 2
    # UTF-8 bytes written as \x escapes come back as characters below 256; text written as UTF-8 is already
    # characters (the file is read as UTF-8)
    return "".join(out)


def strings_in(path, rel):
    with open(path, encoding="utf-8") as f:
        toks = tokens(f.read())
    found = []
    for k, (kind, val, line) in enumerate(toks):
        if kind != "id" or val not in MACROS:
            continue
        if k > 0 and toks[k - 1][1] in (".", "->", "::"):
            continue
        if k + 1 >= len(toks) or toks[k + 1][1] != "(":
            continue
        args, j = [], k + 2
        for _ in range(MACROS[val]):
            parts = []
            while j < len(toks) and toks[j][0] == "str":
                parts.append(toks[j][1])
                j += 1
            if not parts:
                break
            args.append("".join(parts))
            if j < len(toks) and toks[j][1] == ",":
                j += 1
        if len(args) < MACROS[val]:
            continue  # (not a literal: text made at run time is not in the template)
        ref = "%s:%d" % (rel.replace(os.sep, "/"), line)
        if val == "L_":
            vis = args[0].split("##")[0]
            if vis:
                found.append(("", vis, None, ref))
        elif val == "Z_":
            for item in args[0].split("\0"):
                if item:
                    found.append(("", item, None, ref))
        elif val == "P_":
            found.append(("", args[0], args[1], ref))
        elif val == "C_":
            found.append((args[0], args[1], None, ref))
        else:
            found.append(("", args[0], None, ref))
    return found


def collect(root):
    files = []
    for d in ("src",):
        for dirpath, _, names in os.walk(os.path.join(root, d)):
            for nm in names:
                if nm.endswith((".cpp", ".h")):
                    p = os.path.join(dirpath, nm)
                    rel = os.path.relpath(p, root)
                    if rel not in SKIP:
                        files.append((rel, p))
    files.sort()
    entries = {}  # (ctx, id) -> [plural, refs]
    order = []
    for rel, p in files:
        for ctx, mid, plural, ref in strings_in(p, rel):
            key = (ctx, mid)
            if key not in entries:
                entries[key] = [plural, []]
                order.append(key)
            elif plural and not entries[key][0]:
                entries[key][0] = plural
            entries[key][1].append(ref)
    return [(k, entries[k][0], entries[k][1]) for k in order]


def po_string(s):
    esc = s.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t")
    if "\n" not in s.rstrip("\n") or len(s) < 60:
        return '"%s"' % esc.replace("\n", "\\n")
    lines = esc.split("\n")
    parts = [line + ("\\n" if k < len(lines) - 1 else "") for k, line in enumerate(lines)]
    return '""\n' + "\n".join('"%s"' % p for p in parts if p)


def version(root):
    with open(os.path.join(root, "CMakeLists.txt"), encoding="utf-8") as f:
        m = re.search(r"project\(Patina VERSION ([0-9.]+)", f.read())
    return m.group(1) if m else "0"


def template(root):
    out = ["# The text of Patina's interface: the template for translations.",
           "# Make a translation from it (Preferences > Language > Make a new translation, or copy it to <code>.po),",
           "# fill in the msgstr lines, and put the file in your languages folder. Keep the %d / %s codes, in the",
           "# same order, and the \\n line breaks. The manual's \"Translating Patina\" page has the rest.",
           "# This file is made by scripts/extract_strings.py; don't edit it.",
           "#",
           "#, fuzzy",
           'msgid ""',
           'msgstr ""',
           '"Project-Id-Version: Patina %s\\n"' % version(root),
           '"Report-Msgid-Bugs-To: \\n"',
           '"PO-Revision-Date: \\n"',
           '"Last-Translator: \\n"',
           '"Language-Team: \\n"',
           '"Language: \\n"',
           '"MIME-Version: 1.0\\n"',
           '"Content-Type: text/plain; charset=UTF-8\\n"',
           '"Content-Transfer-Encoding: 8bit\\n"',
           '"Plural-Forms: nplurals=2; plural=(n != 1);\\n"',
           '"X-Patina-Language-Name: \\n"',
           ""]
    for (ctx, mid), plural, refs in collect(root):
        line, refl = [], "#:"
        for r in refs:
            if len(refl) + 1 + len(r) > 78 and refl != "#:":
                line.append(refl)
                refl = "#:"
            refl += " " + r
        line.append(refl)
        if FORMAT.search(mid) or (plural and FORMAT.search(plural)):
            line.append("#, c-format")
        if ctx:
            line.append("msgctxt " + po_string(ctx))
        line.append("msgid " + po_string(mid))
        if plural:
            line.append("msgid_plural " + po_string(plural))
            line.append('msgstr[0] ""')
            line.append('msgstr[1] ""')
        else:
            line.append('msgstr ""')
        out.append("\n".join(line))
        out.append("")
    return "\n".join(out)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    root = os.path.abspath(args[0] if args else os.path.join(os.path.dirname(__file__), ".."))
    path = os.path.join(root, "languages", "patina.pot")
    text = template(root)
    count = text.count("\nmsgid ") - 1
    if "--stats" in sys.argv:
        per = {}
        for (ctx, mid), plural, refs in collect(root):
            f = refs[0].split(":")[0]
            per[f] = per.get(f, 0) + 1
        print(count, "texts")
        for f, k in sorted(per.items(), key=lambda x: -x[1])[:40]:
            print("  %5d %s" % (k, f))
        return 0
    if "--check" in sys.argv:
        try:
            with open(path, encoding="utf-8") as f:
                old = f.read()
        except OSError:
            old = ""
        if old != text:
            print("languages/patina.pot is out of date: run scripts/extract_strings.py")
            return 1
        print("languages/patina.pot is up to date (%d texts)" % count)
        return 0
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote", path, "(%d texts)" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
