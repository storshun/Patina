"""Patina Bridge - live link between Blender and the Patina texture painter.

Send a mesh to Patina, paint it there, and get the textures (and edited UVs)
back as a Principled BSDF material - automatically on every save in Patina
when the live link is on, or on demand with *Pull Textures*.

Works as a legacy add-on (``bl_info``) and as a Blender 4.2+ extension
(``blender_manifest.toml``).
"""

bl_info = {
    "name": "Patina Bridge",
    "author": "Patina contributors",
    "version": (0, 14, 0),
    "blender": (3, 6, 0),
    "location": "3D Viewport > Sidebar (N) > Patina",
    "description": "Live link to the Patina texture painter: send meshes, receive textures and UVs",
    "category": "Import-Export",
    "support": "COMMUNITY",
    "doc_url": "",
}

if "bpy" in locals():  # F8 / "Reload Scripts": reload submodules in dependency order
    import importlib

    for _mod in (protocol, server, mesh_io, materials, autosync, library, operators, ui):  # noqa: F821
        importlib.reload(_mod)
    del _mod

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, StringProperty
from bpy.types import AddonPreferences

from . import protocol, server, mesh_io, materials, autosync, library, operators, ui  # noqa: E402,F401


def _restart_listener(self, context):
    server.state.listener_error = ""
    server.restart_listener()


class PatinaBridgePreferences(AddonPreferences):
    """Add-on preferences (Edit > Preferences > Add-ons / Get Extensions > Patina Bridge)."""

    bl_idname = __package__

    patina_executable: StringProperty(
        name="Patina Executable",
        description="Path to the Patina program, or just \"patina\" to look it up on PATH",
        default="patina",
        subtype="FILE_PATH",
    )  # type: ignore[valid-type]
    patina_port: IntProperty(
        name="Patina Port",
        description="TCP port of Patina's command server on 127.0.0.1 (Patina's --port)",
        default=protocol.DEFAULT_PATINA_PORT, min=1024, max=65535,
    )  # type: ignore[valid-type]
    blender_port: IntProperty(
        name="Blender Listen Port",
        description="TCP port on 127.0.0.1 where this add-on receives texture updates from Patina",
        default=protocol.DEFAULT_BLENDER_PORT, min=1024, max=65535,
        update=_restart_listener,
    )  # type: ignore[valid-type]
    exchange_dir: StringProperty(
        name="Exchange Folder",
        description="Folder shared with Patina for meshes and textures "
                    "(empty: $PATINA_BRIDGE_DIR or ~/.cache/patina/bridge)",
        default="~/.cache/patina/bridge",
        subtype="DIR_PATH",
    )  # type: ignore[valid-type]
    default_resolution: EnumProperty(
        name="Texture Resolution",
        description="Resolution of the texture set Patina creates for a newly sent mesh",
        items=[
            ("512", "512", "512 x 512"),
            ("1024", "1024", "1024 x 1024"),
            ("2048", "2048", "2048 x 2048"),
            ("4096", "4096", "4096 x 4096"),
        ],
        default="2048",
    )  # type: ignore[valid-type]
    auto_start: BoolProperty(
        name="Start Live Link on Load",
        description="Start listening for Patina as soon as the add-on is enabled",
        default=True,
    )  # type: ignore[valid-type]
    use_displacement: BoolProperty(
        name="Use Displacement",
        description="Connect the height map to the material output through a Displacement node "
                    "(midlevel 0.5). Off: the height map is loaded but not connected; Patina's normal map "
                    "already contains the height detail",
        default=False,
    )  # type: ignore[valid-type]
    apply_modifiers: BoolProperty(
        name="Apply Modifiers When Sending",
        description="Send the evaluated mesh with all visible modifiers applied. Modifiers that only move "
                    "vertices (Armature, Shrinkwrap, shape keys) keep the UV round trip. Modifiers that add or "
                    "remove faces (Subdivision, Mirror) change the topology, so UVs edited in Patina can NOT be "
                    "written back to the original mesh",
        default=False,
    )  # type: ignore[valid-type]
    replace_materials: BoolProperty(
        name="Show on Every Slot",
        description="Patina paints one texture set for the whole object, so its material replaces the "
                    "material in every slot (the old ones are kept: 'Restore Materials' puts them back). "
                    "Off: it is added as a new slot and faces keep their materials",
        default=True,
    )  # type: ignore[valid-type]

    auto_update_mesh: BoolProperty(
        name="Auto Update Mesh",
        description="After you change a mesh that was sent to Patina (modeling, sculpting, UV edits), send it "
                    "again by itself once the edits pause. Patina keeps the layers and moves the paint",
        default=False,
    )  # type: ignore[valid-type]
    auto_update_delay: FloatProperty(
        name="Settle Time",
        description="Seconds without changes before the mesh is sent (sculpting waits three times longer)",
        default=1.5, min=0.3, max=30.0, subtype="TIME", unit="TIME",
    )  # type: ignore[valid-type]

    library_dir: StringProperty(
        name="Material Library Folder",
        description="Where Add Patina Materials to Blender puts the library (empty: Documents/Patina Materials). "
                    "It is added to Preferences > File Paths > Asset Libraries",
        default="",
        subtype="DIR_PATH",
    )  # type: ignore[valid-type]

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        col = layout.column(heading="Patina")
        col.prop(self, "patina_executable")
        col.prop(self, "patina_port")

        col = layout.column(heading="Bridge")
        col.prop(self, "blender_port")
        col.prop(self, "exchange_dir")
        col.prop(self, "auto_start")

        col = layout.column(heading="Exchange")
        col.prop(self, "default_resolution")
        col.prop(self, "apply_modifiers")
        col.prop(self, "replace_materials")
        col.prop(self, "use_displacement")
        col.prop(self, "auto_update_mesh")
        sub = col.row()
        sub.enabled = self.auto_update_mesh
        sub.prop(self, "auto_update_delay")

        col = layout.column(heading="Materials")
        col.prop(self, "library_dir")
        col.operator("patina.material_library", icon="ASSET_MANAGER")

        st = server.state
        box = layout.box()
        if st.listening:
            box.label(text="Live link listening on 127.0.0.1:%d" % st.listener.port, icon="LINKED")
        elif st.listener_error:
            box.label(text=st.listener_error, icon="ERROR")
        else:
            box.label(text="Live link is off", icon="UNLINKED")
        box.label(text="Exchange folder: %s" % server.exchange_dir(), icon="FILE_FOLDER")


_classes = (PatinaBridgePreferences,) + operators.classes + library.classes + ui.classes


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    server.register()


def unregister():
    try:
        server.unregister()
    finally:
        for cls in reversed(_classes):
            try:
                bpy.utils.unregister_class(cls)
            except RuntimeError:
                pass


if __name__ == "__main__":
    register()
