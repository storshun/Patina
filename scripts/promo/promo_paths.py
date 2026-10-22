"""Where the promo kit reads and writes: the repo, and a work folder for renders (git ignores build-*)."""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = Path(os.environ.get("PATINA_PROMO_WORK", ROOT / "build-promo"))
EXE = Path(os.environ.get("PATINA_BIN", ROOT / "build-release" / "patina"))
BRAND = ROOT / "docs" / "brand"
MANUAL = ROOT / "docs" / "manual" / "img"
