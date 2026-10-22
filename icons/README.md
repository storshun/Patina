# Patina's icons

The toolbar icons (0.36). Each `.svg` here is built into Patina (`cmake/EmbedIcons.cmake`), read by
`src/core/VectorIcon.cpp` and drawn by `src/ui/Icons.cpp` at any size. The file name is the icon's name:
`iconButton(id, "seams", ...)` draws `seams.svg`. The design canvas is on claude.ai (Claude Design).

## The style

- A 24 by 24 grid (`viewBox="0 0 24 24"`), shapes inside 2.5 to 21.5.
- Lines, not fills: `stroke-width="1.75"`, `stroke-linecap="round"`, `stroke-linejoin="round"`. A fill only for a
  small solid part (an eyedropper's drop, the dot of an eye).
- `currentColor` for the shape: it takes the button's color (light, dark on a chosen tool, faded when off).
- `#E8852C`, Patina's copper, for the one part that does the work (the brush tip, the eraser's rubber, the
  scissors' cut). Keep it to one part.
- Axis letters in Blender's colors: X `#F0525F`, Y `#7DC242`, Z `#4A90F0` (the mirror buttons and the axis
  gizmo use the same ones).
- Borrow the shape other programs use for the same job (Photoshop, Krita, Blender, Substance). People should not
  have to learn a new picture. Avoid shapes that already mean something else (a globe reads as "language" or "web").

## What the reader understands

`svg`, `g`, `path` (every command, arcs too), `circle`, `ellipse`, `rect` (with `rx`), `line`, `polyline`,
`polygon`; `transform` (translate, rotate, scale, matrix); `stroke`, `fill`, `stroke-width`, `stroke-opacity`,
`fill-opacity`, `opacity`, `stroke-dasharray`. Anything else (text, gradients, `style`) is reported as a problem,
not guessed. The `core/toolbar_icons` test checks every file here.

## Adding one

1. Draw it in the style above and save it here as `name.svg`. CMake picks it up on the next build.
2. Draw it with `App::iconButton` (`src/ui/Toolbars.cpp`), or `ui::drawIcon` for an icon that isn't a button.
3. Give the button a name and a tip (they show on hover and translate), and a keymap id if it has a shortcut.
4. Add it to the icon tables in `docs/manual/src/03-interface.html` as `[[icon:name]]`.
