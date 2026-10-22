"""Patina's material library in Blender's Asset Browser.

*Add Patina Materials to Blender* asks Patina to bake every library material
(``export_material_maps``; a hidden Patina is started when none is running),
then a background Blender builds ``Patina Materials.blend`` from the maps (see
``library_build.py``) and the folder is added to *Preferences > File Paths >
Asset Libraries*. Drag a material from the Asset Browser onto any object.

Running it again brings the library up to date (new materials, your own
materials, a different map size or mapping). Materials already used in a
.blend file point at the same map files, so they update as well.
"""

from __future__ import annotations

import os
import re
import socket
import subprocess
import threading
import time
from typing import Any, Dict, Optional

import bpy
from bpy.props import EnumProperty
from bpy.types import Operator

from . import protocol, server

LIBRARY_NAME = "Patina Materials"
BLEND_NAME = "Patina Materials.blend"


def default_dir() -> str:
    home = os.path.expanduser("~")
    docs = os.path.join(home, "Documents")
    return os.path.join(docs if os.path.isdir(docs) else home, LIBRARY_NAME)


def library_dir() -> str:
    raw = (server.pref("library_dir", "") or "").strip()
    if not raw:
        return default_dir()
    if raw.startswith("//"):
        raw = bpy.path.abspath(raw)
    return os.path.abspath(os.path.expanduser(raw))


class _Job:
    def __init__(self) -> None:
        self.step = "Starting..."
        self.done = False
        self.error = ""
        self.count = 0
        self.started = time.monotonic()
        self.directory = ""


job: Optional[_Job] = None


def running() -> bool:
    return job is not None and not job.done


def status_text() -> str:
    if job is None:
        return ""
    if not job.done:
        return "%s (%d s)" % (job.step, int(time.monotonic() - job.started))
    if job.error:
        return "Material library: " + job.error
    return "%d Patina materials are in the Asset Browser (%s)" % (job.count, LIBRARY_NAME)


def _free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind((protocol.LOCALHOST, 0))
        return s.getsockname()[1]
    finally:
        s.close()


def _hidden() -> Dict[str, Any]:
    if os.name == "nt":
        return {"creationflags": getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)}
    return {}


def _export_headless(exe: str, args: Dict[str, Any]) -> Any:
    """No Patina is running: start a hidden one just for the export."""
    port = _free_port()
    proc = subprocess.Popen([exe, "--headless", "--port", str(port)], stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            cwd=os.path.dirname(exe) or None, **_hidden())
    try:
        deadline = time.monotonic() + 60.0
        while True:
            if proc.poll() is not None:
                raise RuntimeError("Patina stopped with code %s before it could bake the materials" % proc.returncode)
            try:
                return protocol.request_once("export_material_maps", args, port=port, connect_timeout=1.0,
                                             timeout=3600.0)
            except (protocol.BridgeConnectionError, ConnectionError, OSError):
                if time.monotonic() > deadline:
                    raise RuntimeError("Patina did not start (%s)" % exe)
                time.sleep(0.5)
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()


def _run(j: _Job, p: Dict[str, Any]) -> None:
    try:
        maps = os.path.join(p["dir"], "maps")
        os.makedirs(maps, exist_ok=True)
        args = {"directory": maps, "size": p["size"], "format": "jpg", "quality": 92, "which": p["which"],
                "height16": True}
        j.step = "Patina is baking the materials"
        try:
            protocol.request_once("ping", {}, port=p["port"], connect_timeout=3.0, timeout=15.0)
            live = True
        except (protocol.BridgeConnectionError, ConnectionError, OSError):
            live = False
        if live:
            protocol.request_once("export_material_maps", args, port=p["port"], connect_timeout=3.0, timeout=3600.0)
        else:
            if not p["exe"]:
                raise RuntimeError("Patina is not running and the add-on can't find it: set the Patina executable "
                                   "in the add-on preferences")
            j.step = "A hidden Patina is baking the materials"
            _export_headless(p["exe"], args)
        j.step = "Blender is building the library"
        out = os.path.join(p["dir"], BLEND_NAME)
        cmd = [p["blender"], "-b", "--factory-startup", "--python", p["script"], "--", maps, out, p["mapping"]]
        r = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=3600, **_hidden())
        text = r.stdout.decode("utf-8", "replace")
        m = re.search(r"PATINA_LIBRARY_DONE (\d+)", text)
        if not m:
            e = re.search(r"PATINA_LIBRARY_ERROR (.*)", text)
            print(text[-4000:])
            raise RuntimeError(e.group(1).strip() if e else "the library build stopped (code %d)" % r.returncode)
        j.count = int(m.group(1))
    except Exception as exc:  # noqa: BLE001 - shown in the panel
        j.error = str(exc) or type(exc).__name__
    finally:
        j.done = True


def register_library(directory: str) -> str:
    """Adds (or points) the "Patina Materials" asset library at ``directory``. Main thread."""
    prefs = bpy.context.preferences
    libs = prefs.filepaths.asset_libraries
    norm = os.path.normcase(os.path.normpath(directory))
    lib = next((l for l in libs if os.path.normcase(os.path.normpath(bpy.path.abspath(l.path))) == norm), None)
    if lib is None:
        lib = next((l for l in libs if l.name == LIBRARY_NAME), None)
    if lib is None:
        try:
            lib = libs.new(name=LIBRARY_NAME, directory=directory)
        except TypeError:  # older signature
            lib = libs.new()
            lib.name = LIBRARY_NAME
    lib.path = directory  # Blender's default import method (Append Reuse, or Pack in 5.x) keeps the maps found
    note = ""
    if not getattr(prefs, "use_preferences_save", True):
        note = " (Preferences are not saved automatically: save them to keep the library)"
    return note


def _poll() -> Optional[float]:
    j = job
    server.tag_redraw()
    if j is None:
        return None
    if not j.done:
        return 0.5
    if j.error:
        server.notify("ERROR", "Patina material library: " + j.error)
        return None
    try:
        note = register_library(j.directory)
    except Exception as exc:  # noqa: BLE001
        server.notify("ERROR", "The library was built in %s but could not be added to the Asset Browser: %s"
                      % (j.directory, exc))
        return None
    server.notify("INFO", "%d Patina materials are in the Asset Browser under \"%s\"%s"
                  % (j.count, LIBRARY_NAME, note), popup=False)
    return None


def start(size: int = 1024, mapping: str = "BOX", which: str = "all") -> str:
    """Starts building the library in the background. Returns "" or why it can't start. Main thread."""
    global job
    if running():
        return "the material library is already being built"
    try:
        exe = server.resolve_executable(server.pref("patina_executable", "patina"))
    except FileNotFoundError:
        exe = ""
    directory = library_dir()
    try:
        os.makedirs(directory, exist_ok=True)
    except OSError as exc:
        return "can't make the folder %s: %s" % (directory, exc)
    j = _Job()
    j.directory = directory
    params = {"dir": directory, "size": int(size), "mapping": mapping.upper(), "which": which,
              "port": server.patina_port(), "exe": exe, "blender": bpy.app.binary_path,
              "script": os.path.join(os.path.dirname(os.path.abspath(__file__)), "library_build.py")}
    job = j
    if bpy.app.background:
        _run(j, params)
        _poll()
        return j.error
    threading.Thread(target=_run, args=(j, params), name="PatinaMaterialLibrary", daemon=True).start()
    if not bpy.app.timers.is_registered(_poll):
        bpy.app.timers.register(_poll, first_interval=0.5, persistent=True)
    return ""


class PATINA_OT_material_library(Operator):
    """Put Patina's material library in Blender's Asset Browser (baked by Patina; run again to update it)"""

    bl_idname = "patina.material_library"
    bl_label = "Add Patina Materials to Blender"
    bl_options = {"REGISTER"}

    size: EnumProperty(
        name="Map Size",
        items=[("512", "512", "Quick, about 80 MB"), ("1024", "1024", "About 300 MB for all materials"),
               ("2048", "2048", "Sharp close-ups, about 1.2 GB")],
        default="1024",
    )  # type: ignore[valid-type]
    mapping: EnumProperty(
        name="Mapping",
        items=[("BOX", "Box (like Patina)", "Projected from the sides, top and front: no UVs needed, the same size "
                "on every object (tiles per meter)"),
               ("UV", "UV", "Follows the object's UV map (tiles per UV square), with Patina's normal maps")],
        default="BOX",
    )  # type: ignore[valid-type]
    which: EnumProperty(
        name="Materials",
        items=[("all", "Patina's and Mine", "The built-in library and the materials you made in Patina"),
               ("builtin", "Patina's Only", "The built-in library")],
        default="all",
    )  # type: ignore[valid-type]

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=360)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.prop(self, "size")
        layout.prop(self, "mapping")
        layout.prop(self, "which")
        layout.label(text="Folder: " + library_dir(), icon="FILE_FOLDER")

    def execute(self, context):
        err = start(int(self.size), self.mapping, self.which)
        if err:
            self.report({"ERROR"}, err)
            return {"CANCELLED"}
        self.report({"INFO"}, "Building the Patina material library in the background")
        return {"FINISHED"}


classes = (PATINA_OT_material_library,)
