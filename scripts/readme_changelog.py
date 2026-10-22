#!/usr/bin/env python3
"""Copies the change log's entries from 1.0.0 on into the README's "What's new", between its changelog markers.
The newest version is written out; the ones before it fold away under their headings.

    python3 scripts/readme_changelog.py           # update README.md
    python3 scripts/readme_changelog.py --check   # exit 1 when README.md is out of date (ctest readme_changelog)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
START = "<!-- changelog: filled from CHANGELOG.md by scripts/readme_changelog.py -->"
END = "<!-- /changelog -->"
OLDEST = (1, 0, 0)


def versions(text):
    """[(version tuple, heading, body)] for each '## x.y.z (date)' section, newest first."""
    out = []
    parts = re.split(r"^## ", text, flags=re.M)
    for part in parts[1:]:
        head, _, body = part.partition("\n")
        m = re.match(r"(\d+)\.(\d+)\.(\d+)", head)
        if m:
            out.append((tuple(int(x) for x in m.groups()), head.strip(), body.strip("\n")))
    return out


def render(text):
    blocks = []
    entries = [e for e in versions(text) if e[0] >= OLDEST]
    for i, (_, head, body) in enumerate(entries):
        body = re.sub(r"^### ", "#### ", body, flags=re.M)
        if i == 0:
            blocks.append(f"### {head}\n\n{body}")
        else:  # GitHub renders Markdown inside <details> when blank lines surround it
            blocks.append(f"<details>\n<summary><b>{head}</b></summary>\n\n{body}\n\n</details>")
    return "\n\n".join(blocks)


def main():
    readme = os.path.join(ROOT, "README.md")
    text = open(readme, encoding="utf-8").read()
    log = open(os.path.join(ROOT, "CHANGELOG.md"), encoding="utf-8").read()
    a, b = text.find(START), text.find(END)
    if a < 0 or b < a:
        raise SystemExit("README.md has no changelog markers")
    new = text[:a + len(START)] + "\n" + render(log) + "\n" + text[b:]
    if "--check" in sys.argv:
        if new != text:
            print("README.md's What's new is out of date: run python3 scripts/readme_changelog.py")
            return 1
        print("README.md's What's new matches CHANGELOG.md")
        return 0
    if new != text:
        open(readme, "w", encoding="utf-8").write(new)
        print("README.md updated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
