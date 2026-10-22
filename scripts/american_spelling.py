"""Patina is written in American English. This finds British spellings (and with --apply fixes them): standalone
words everywhere (prose, comments, strings, plain identifiers) and the British part of camelCase identifiers.
snake_case tokens too (0.36: the material ids were renamed; old keys and ids live on as aliases on KEEP lines), not the Assistant's files
(its trained wording and the British words it understands), lines that keep an old spelling on purpose (KEEP), and
'cancelled' (an API value; prose says canceled). ctest runs it as the test "spelling".

    python3 scripts/american_spelling.py [ROOT] [--apply]
"""
import os
import re
import sys

WORDS = {
    "colour": "color", "colours": "colors", "coloured": "colored", "colourful": "colorful", "colourfulness": "colorfulness", "colouring": "coloring",
    "recolour": "recolor", "colourways": "colorways", "colourway": "colorway", "discoloured": "discolored",
    "centre": "center", "centres": "centers", "centred": "centered", "centring": "centering",
    "metre": "meter", "metres": "meters", "centimetre": "centimeter", "centimetres": "centimeters",
    "millimetre": "millimeter", "millimetres": "millimeters", "kilometre": "kilometer", "kilometres": "kilometers",
    "grey": "gray", "greys": "grays", "greyed": "grayed", "greyscale": "grayscale", "greyish": "grayish",
    "neighbour": "neighbor", "neighbours": "neighbors", "neighbouring": "neighboring",
    "stylised": "stylized", "stylise": "stylize", "organising": "organizing", "organise": "organize",
    "organised": "organized", "organisation": "organization",
    "normalise": "normalize", "normalised": "normalized", "normalises": "normalizes", "normalising": "normalizing",
    "normalisation": "normalization", "galvanised": "galvanized", "oxidised": "oxidized", "oxidise": "oxidize",
    "anodised": "anodized", "rasterise": "rasterize", "rasterised": "rasterized", "rasterises": "rasterizes",
    "rasterising": "rasterizing", "recognise": "recognize", "recognisable": "recognizable", "unrecognised": "unrecognized", "recognised": "recognized", "recognises": "recognizes",
    "recognising": "recognizing", "equalise": "equalize", "equalised": "equalized", "equalising": "equalizing",
    "serialise": "serialize", "serialised": "serialized", "serialises": "serializes", "serialising": "serializing",
    "analyse": "analyze", "analysed": "analyzed", "analysing": "analyzing",
    "initialise": "initialize", "initialised": "initialized", "maximise": "maximize", "maximises": "maximizes",
    "minimise": "minimize", "minimises": "minimizes", "optimise": "optimize", "optimised": "optimized",
    "optimisation": "optimization", "memoised": "memoized", "parameterisation": "parameterization",
    "quantise": "quantize", "quantised": "quantized", "quantising": "quantizing", "symmetrise": "symmetrize",
    "factorisation": "factorization", "visualise": "visualize", "customise": "customize", "customised": "customized",
    "summarise": "summarize", "realise": "realize", "realised": "realized", "finalise": "finalize",
    "prioritise": "prioritize", "randomise": "randomize", "randomised": "randomized", "synchronise": "synchronize",
    "utilise": "utilize", "voxelise": "voxelize", "voxelised": "voxelized", "linearise": "linearize",
    "modelling": "modeling", "modelled": "modeled", "modeller": "modeler", "modellers": "modelers",
    "bevelled": "beveled", "bevelling": "beveling", "labelled": "labeled", "labelling": "labeling",
    "travelled": "traveled", "travelling": "traveling", "stencilled": "stenciled", "panelling": "paneling",
    "panelled": "paneled", "levelled": "leveled", "levelling": "leveling", "tunnelling": "tunneling",
    "behaviour": "behavior", "behaviours": "behaviors", "armour": "armor", "armoured": "armored",
    "fibre": "fiber", "fibres": "fibers", "licence": "license", "licences": "licenses", "catalogue": "catalog",
    "catalogues": "catalogs", "programme": "program", "jewellery": "jewelry", "aluminium": "aluminum",
    "anticlockwise": "counterclockwise", "towards": "toward", "afterwards": "afterward", "maths": "math",
    "favour": "favor", "favourite": "favorite", "honour": "honor", "flavour": "flavor", "humour": "humor",
    "mould": "mold", "moulds": "molds", "kerb": "curb", "kerbs": "curbs", "kerbstone": "curbstone",
    "kerbstones": "curbstones", "tyre": "tire", "tyres": "tires", "whilst": "while", "amongst": "among",
    "learnt": "learned", "spelt": "spelled", "artefact": "artifact", "artefacts": "artifacts", "ageing": "aging",
    "judgement": "judgment", "dialogue": "dialog", "analogue": "analog", "plough": "plow", "draught": "draft",
    "lustre": "luster", "calibre": "caliber", "theatre": "theater", "sombre": "somber", "manoeuvre": "maneuver",
    "defence": "defense", "practise": "practice", "enquiry": "inquiry", "storey": "story",
}
# the British part of a camelCase identifier (faceColour, uGenCentre, SwapColours, unitToMetres, colourTag)
CAMEL = [("Colours", "Colors"), ("Colour", "Color"), ("Centres", "Centers"), ("Centre", "Center"),
         ("Neighbours", "Neighbors"), ("Neighbour", "Neighbor"), ("Greyscale", "Grayscale"), ("Grey", "Gray"),
         ("Metres", "Meters"), ("Metre", "Meter"), ("Normalise", "Normalize"), ("Rasterise", "Rasterize"),
         ("Serialise", "Serialize"), ("Behaviour", "Behavior"), ("Labelled", "Labeled"), ("Modelling", "Modeling")]
LOWER_PREFIX = [("colours", "colors"), ("colour", "color"), ("centre", "center"), ("neighbours", "neighbors"),
                ("neighbour", "neighbor"), ("grey", "gray"), ("metres", "meters")]

SKIP_FILES = {"src/engine/AutoTexture.cpp", "src/engine/AssistantContext.cpp", "src/engine/AssistantContext.h",
              "src/engine/Assistant.cpp", "src/engine/Assistant.h", "src/engine/AssistantModel.cpp",
              "src/engine/AssistantModel.h", "src/engine/CommandsAssistant.cpp", "src/engine/MapImport.cpp",
              "tests/test_assistant.cpp", "scripts/american_spelling.py"}
SKIP_DIRS = ("third_party", "build", "dist", "web/dist", "docs/manual/img", "__pycache__", ".git", ".cache",
             "node_modules")
EXTS = (".cpp", ".h", ".glsl", ".py", ".md", ".html", ".js", ".mjs", ".json", ".txt", ".desktop", ".css", ".sh", ".in", ".toml")
ROOTS = ["src", "docs", "README.md", "materials", "smart_materials", "blender", "web/src", "web/index.html",
         "scripts", "tests", "recipes", "packaging", "CMakeLists.txt", "third_party/LICENSES.md", "languages/README.md",
         "icons", "cmake"]

# (a word also starts after a C escape such as \0 or \n: "Smooth\0Stylised\0" in a combo list)
word_re = re.compile(r"(?:\b|(?<=\\[0nrt]))(" + "|".join(sorted(WORDS, key=len, reverse=True)) + r")\b", re.I)
# (0.36) snake_case and ids too (material ids, keys): the old ones live on only on lines marked KEEP
snake_re = re.compile(r"(?<=_)(" + "|".join(sorted(WORDS, key=len, reverse=True)) + r")(?=_|\b)|\b(" +
                      "|".join(sorted(WORDS, key=len, reverse=True)) + r")(?=_)", re.I)
camel_re = re.compile(r"(?<=[a-z0-9])(" + "|".join(a for a, _ in CAMEL) + r")(?![a-z])")
lower_re = re.compile(r"\b(" + "|".join(a for a, _ in LOWER_PREFIX) + r")(?=[A-Z])")


def case_like(src, dst):
    if src.isupper() and len(src) > 1:
        return dst.upper()
    if src[0].isupper():
        return dst[0].upper() + dst[1:]
    return dst


KEEP = ("either spelling", "the old name", "0.32 to 0.35 name", "`centre`", "`stylised`", "the old id")  # old spellings on purpose


def convert(text):
    lines = text.split("\n")
    out, total = [], 0
    for line in lines:
        if any(k in line for k in KEEP):
            out.append(line)
            continue
        line, n = convert_line(line)
        out.append(line)
        total += n
    return "\n".join(out), total


def convert_line(text):
    n = [0]

    def w(m):
        n[0] += 1
        return case_like(m.group(1), WORDS[m.group(1).lower()])

    text = word_re.sub(w, text)

    def sn(m):
        n[0] += 1
        g = m.group(1) or m.group(2)
        return case_like(g, WORDS[g.lower()])

    text = snake_re.sub(sn, text)

    def c(m):
        n[0] += 1
        return dict(CAMEL)[m.group(1)]

    text = camel_re.sub(c, text)

    def l(m):
        n[0] += 1
        return dict(LOWER_PREFIX)[m.group(1)]

    text = lower_re.sub(l, text)
    return text, n[0]


def files(root):
    for r in ROOTS:
        p = os.path.join(root, r)
        if os.path.isfile(p):
            yield r
            continue
        for dp, dn, fn in os.walk(p):
            rel = os.path.relpath(dp, root)
            dn[:] = [d for d in dn if not any(os.path.join(rel, d).startswith(s) or d == s for s in SKIP_DIRS)]
            for f in fn:
                if f.endswith(EXTS):
                    yield os.path.join(rel, f)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    root = args[0] if args else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    apply = "--apply" in sys.argv
    total = 0
    for rel in sorted(set(files(root))):
        if rel in SKIP_FILES:
            continue
        p = os.path.join(root, rel)
        try:
            t = open(p, encoding="utf-8").read()
        except (UnicodeDecodeError, OSError):
            continue
        out, n = convert(t)
        if n:
            total += n
            print("%5d  %s" % (n, rel))
            if apply:
                open(p, "w", encoding="utf-8").write(out)
    print("total", total)
    if total and not apply:
        print("British spellings above: run with --apply to fix them (or mark a line that keeps one on purpose)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
