"""The "Patina" tab in the 3D viewport sidebar (``PATINA_PT_*``)."""

import time

import bpy
from bpy.types import Panel

from . import mesh_io, server


class PATINA_PT_main(Panel):
    """Live link to the Patina texture painter"""

    bl_idname = "PATINA_PT_main"
    bl_label = "Patina"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Patina"

    def draw(self, context):
        layout = self.layout
        st = server.state

        # ---- status
        box = layout.box()
        col = box.column(align=True)
        online = st.patina_online
        recent = st.last_contact and time.monotonic() - st.last_contact < 15.0
        if server.is_launching():
            col.label(text="Starting Patina...", icon="SORTTIME")
        elif online or (online is None and recent):
            version = st.patina_info.get("version") if isinstance(st.patina_info, dict) else None
            col.label(text="Patina connected" + (" (%s)" % version if version else ""), icon="LINKED")
        elif online is False:
            col.label(text="Patina not running (port %d)" % server.patina_port(), icon="UNLINKED")
        else:
            col.label(text="Patina: checking...", icon="QUESTION")
        if st.listening:
            col.label(text="Listening on 127.0.0.1:%d" % st.listener.port, icon="CHECKMARK")
        elif st.listener_error:
            col.label(text=st.listener_error, icon="ERROR")
        else:
            col.label(text="Live link off", icon="RADIOBUT_OFF")
        if server.worker.pending:
            col.label(text="Waiting for Patina (%d)..." % server.worker.pending, icon="SORTTIME")

        row = box.row(align=True)
        row.operator("patina.live_link", text="Live Link", icon="LINKED" if st.listening else "UNLINKED",
                     depress=st.listening).action = "TOGGLE"
        row.operator("patina.ping", text="", icon="FILE_REFRESH")

        # ---- actions
        obj = context.active_object
        col = layout.column(align=True)
        col.scale_y = 1.2
        meshes = [o for o in (getattr(context, "selected_objects", ()) or ()) if o.type == "MESH"]
        if obj is not None and obj.type == "MESH" and len(meshes) > 1:
            col.label(text="%s and %d more" % (obj.name, len(meshes) - 1), icon="MESH_DATA")
        elif obj is not None and obj.type == "MESH":
            col.label(text=obj.name, icon="MESH_DATA")
        else:
            col.label(text="Select a mesh object", icon="INFO")
        col.operator("patina.send", text="Send to Patina", icon="EXPORT")
        col.operator("patina.add_to_scene", text="Add to the Patina Scene", icon="ADD")
        col.operator("patina.update_mesh", text="Update Mesh in Patina", icon="FILE_REFRESH")
        prefs = server.get_prefs(context)
        if prefs is not None:
            row = col.row(align=True)
            row.prop(prefs, "auto_update_mesh", text="Auto Update Mesh", toggle=True,
                     icon="REC" if prefs.auto_update_mesh else "TIME")
            from . import autosync

            if prefs.auto_update_mesh and obj is not None and obj.name in autosync.pending():
                col.label(text="Changed: sending when the edits pause", icon="SORTTIME")
        col.operator("patina.pull", text="Pull Textures", icon="IMPORT")

        layout.operator("patina.launch", text="Launch Patina", icon="PLAY")

        # ---- per-object notes
        if obj is not None and obj.type == "MESH":
            prefs = server.get_prefs(context)
            notes = []
            sent = server.state.sent.get(obj.name, {})
            if sent.get("evaluated"):
                notes.append(("Sent with modifiers applied: UV edits can't return", "MODIFIER"))
            elif prefs is not None and prefs.apply_modifiers and mesh_io.has_active_modifiers(obj) and not sent:
                notes.append(("Modifiers will be applied when sending", "MODIFIER"))
            if obj.data.uv_layers.active is None:
                notes.append(("No UV map - unwrap in Patina", "UV"))
            for text, icon in notes:
                layout.label(text=text, icon=icon)
            from . import materials

            if materials.has_original_materials(obj):
                layout.operator("patina.restore_materials", icon="MATERIAL")
            if obj.name in server.state.sent:
                layout.operator("patina.check_symmetry", icon="MOD_MIRROR")

        # ---- last message
        if st.message:
            icon = {"ERROR": "ERROR", "WARNING": "ERROR"}.get(st.message_level, "INFO")
            msg_box = layout.box()
            _wrapped(msg_box, st.message, icon, context)


def _wrapped(layout, text: str, icon: str, context) -> None:
    """Draw ``text`` wrapped to the panel width."""
    try:
        width = max(16, int(context.region.width / (7.0 * context.preferences.system.ui_scale)) - 4)
    except Exception:
        width = 40
    words, line, lines = text.split(), "", []
    for w in words:
        if line and len(line) + 1 + len(w) > width:
            lines.append(line)
            line = w
        else:
            line = (line + " " + w).strip()
    if line:
        lines.append(line)
    col = layout.column(align=True)
    for i, ln in enumerate(lines[:8]):
        col.label(text=ln, icon=icon if i == 0 else "BLANK1")


class PATINA_PT_settings(Panel):
    bl_idname = "PATINA_PT_settings"
    bl_label = "Settings"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Patina"
    bl_parent_id = "PATINA_PT_main"
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        prefs = server.get_prefs(context)
        if prefs is None:
            layout.label(text="Preferences unavailable", icon="ERROR")
            return
        layout.use_property_split = True
        layout.use_property_decorate = False
        layout.prop(prefs, "default_resolution")
        layout.prop(prefs, "apply_modifiers")
        layout.prop(prefs, "replace_materials")
        layout.prop(prefs, "use_displacement")
        layout.prop(prefs, "auto_update_mesh")
        row = layout.row()
        row.enabled = prefs.auto_update_mesh
        row.prop(prefs, "auto_update_delay")
        layout.operator("patina.open_exchange", icon="FILE_FOLDER")


class PATINA_PT_library(Panel):
    bl_idname = "PATINA_PT_library"
    bl_label = "Material Library"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Patina"
    bl_parent_id = "PATINA_PT_main"

    def draw(self, context):
        from . import library

        layout = self.layout
        col = layout.column(align=True)
        col.enabled = not library.running()
        col.operator("patina.material_library", text="Add Patina Materials to Blender" if not library.job
                     else "Update Patina Materials", icon="ASSET_MANAGER")
        text = library.status_text()
        if text:
            icon = "SORTTIME" if library.running() else ("ERROR" if library.job.error else "CHECKMARK")
            _wrapped(layout.box(), text, icon, context)
        else:
            _wrapped(layout, "Every Patina material in the Asset Browser, ready to drag onto objects.", "INFO",
                     context)


classes = (PATINA_PT_main, PATINA_PT_library, PATINA_PT_settings)
