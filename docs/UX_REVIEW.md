# Patina's interface before 1.0: what the competition gets wrong, and a guided way in

Written for Patina 0.46.0 (2026-10-21), at Randall's request:

> "do some deep diving into our competitors and their UI. Find out things that people don't like about their UI that we
> might be replicating. And also ... look at our user interface from a user perspective and figure out if we are laying
> things out in the way that makes the most sense for someone who has never used the application before, but is coming
> from Blender or coming from Maya ... Things should feel very logical in their flow from one step of the process to the
> next ... figure out the things that we need to get in front of the user in sort of an experiential flow that makes good
> sense as a simplified experience. And then we have the current layout as the kind of advanced custom experience."

What went into it:

- What users say about Substance 3D Painter, Mari, 3DCoat, ArmorPaint, Blender's own texture painting, Marmoset Toolbag,
  InstaMAT and Quixel Mixer, from Steam reviews, Adobe's forums, BlenderArtists, the vendors' forums and docs, and
  reviews.
  - Reddit and Polycount block automated reading, so those come only from search excerpts and carry less weight.
- How other creative apps do a simple mode next to the full one: DaVinci Resolve's pages and Cut page, Affinity's
  personas, Blender's workspaces, Photoshop Elements' Quick / Guided / Expert, Premiere's Import / Edit / Export header,
  Mari's dropped Basic mode, Fusion's workspaces, Office's adaptive menus. Also the research on progressive disclosure,
  wizards and command search.
- A first-time user's walk through Patina 0.45: screenshots of a fresh start, every menu and every panel, and a map of
  every menu item, panel, toolbar, right-click menu and dialog in the code.

## 1. What users dislike in the competition, and where Patina stands

Most common first. "0.46" marks what this version changes.

| What users dislike | Where it comes from | Patina |
|---|---|---|
| **Crashes and slowness with big texture sets, UDIMs and many layers.** The top complaint in Painter's Steam reviews (16 of 52 negative 2026 reviews). | Painter, Toolbag, InstaMAT | Region painting (0.44.2) keeps big sets at 60 fps. Keep testing with Randall's real projects. |
| **Material slots are walls.** Each material is a texture set of its own: you can't paint across them or add a channel to all at once. | Painter | Fine already: one layer stack paints every texture set, and strokes cross from one to the next. |
| **Baking is buried and goes stale.** Painter's bake moved to a hidden "Baking mode" its own docs say "can be hidden behind other panels"; generators silently need maps; Blender bakes into whatever image node is active and can overwrite sources. | Painter, Blender, InstaMAT, ArmorPaint | Was split between the Bake menu and the Texture Set panel. **0.46: Bake is a step of its own**, saying which maps are missing or out of date. Smart masks already bake what they need; bakes are layers, never written over your images. |
| **Export and color-space traps.** The normal map's format is set in three places, overrides drop to 8 bits, export won't overwrite, and Blender users get wrong normals and must set Non-Color by hand. | Painter, Blender, Mari | Presets per engine (Blender, glTF, Unreal, Unity) and a bridge that sets up Blender's nodes. **0.46: the Export step**, and "Your textures go to" on the first start picks the preset. Still open: list every file the export writes (name, packing, bit depth). |
| **"My brush does nothing."** A mask selected instead of the layer, a hidden layer, no channels, the wrong texture set, and no message why. Adobe only added a "forbidden" cursor this month. | Painter | **0.46: the 3D view says why**: no layer, a hidden layer or folder, opacity 0, every channel off. |
| **Updates that break muscle memory.** Painter's 7.2 reset shelves, shortcuts and preferences; 8.1 removed Alt+wheel zoom. | Painter | Patina keeps keys, layouts and settings across versions. **0.46 keeps it so: a Patina used before opens in the Full layout as it was; only new installs start Guided.** |
| **Navigation and keys from another program.** Alt-only orbit, a pivot that traps you inside the mesh, keys that can't be rebound; a user's first job in a new painter is remapping navigation to Blender's. | Painter, 3DCoat, Mixer, Toolbag | Every key rebinds, with Blender, Maya and ZBrush navigation. But the first start only offered Patina or Blender keys. **0.46: "You come from: Blender / Maya / something else"** on the first start. |
| **Tools and data stuck in "rooms".** 3DCoat's rooms hide tools per room and trap work in one room; even its regulars ask for "tools not rooms". | 3DCoat | **0.46's steps are layouts over one project, never walls**: every tool's key, every menu and Search work in every step, and choosing another step's tool goes to that step. |
| **Hard to find things / too many panels / text too small.** "Tools buried under tools" (3DCoat), the minimal UI where users couldn't find import or layers (ArmorPaint), panels gone missing (Painter's "Reset UI"). | 3DCoat, ArmorPaint, Painter, Mari | See section 3: the same problems were in Patina. **0.46** answers them with the Guided layout. |
| **Setup before the first stroke.** Blender needs UVs, a material, an image and a paint slot first; Mari 4 had to automate its project start. | Blender, Mari | Fine already: open a model and paint; texture sets, channels and color spaces are set for you. |
| **Lost work.** Painted images not saved with the .blend; "Lost like 5 hours of progress" (Ucupaint). | Blender | Autosave and crash recovery were there, but **File › New, Open, Open Recent, the welcome buttons and a dropped file replaced unsaved work without asking.** **0.46: they ask (Save / Don't save / Cancel).** |
| **Irreversible project settings.** Painter's UDIM choice can't change after the project is made. | Painter | Fine already: texture sets and UDIMs switch at any time. |

What users praise, and Patina should keep: smart materials and smart masks; a non-destructive, Photoshop-like layer
stack people learn in hours; a real-time PBR view; dragging materials onto the model; baking inside the app; engine
presets; Toolbag's baker (live preview, cage painted in the view, bake groups by name); ArmorPaint's speed and small
size. And Blender users want a Painter-Blender bridge that Adobe doesn't ship: Patina has one.

## 2. Simple modes in other apps: what works and what backfires

- **Tabs in the order of the work, all clickable, with the same tools behind them, work.** DaVinci Resolve's pages
  (Media to Deliver) are the best-known example. Affinity's personas, Blender's workspaces and Fusion's workspaces are
  the same idea.
- **A simple mode that is a different program backfires.**
  - Resolve's Cut page has tools the Edit page lacks and the other way round. Reviewers wanted one set, and beginner
    guides still teach the Edit page.
  - Affinity loses Develop settings when you leave that persona.
  - Final Cut Pro X's simplified rebuild was mocked as "iMovie Pro" until features came back.
  - Mari dropped its Basic mode.
- **Hiding what people don't use makes things unpredictable.** Office 2000's adaptive menus were turned off by default
  in Office 2007.
- **Research.** A minimal interface helps beginners find the core features ("training wheels", Carroll 1984; Findlater
  and McGrenere 2007). But users who start on the full one learn more of the advanced features. So both layouts are
  worth having, and moving between them must cost nothing.
- **Wizards suit beginners and rare tasks, but get in the way of creative work** (NN/g). The guidance:
  - show the list of steps
  - label the button with the next step's name
  - never lock the rest of the app
  - keep it to two levels: the step, then "more options"
- **Tip-card tutorials up front don't help**: in a 70-user study, tasks were rated harder with them (NN/g). Empty states
  and checks that link straight to the fix do help.
- **Floating "next step" bars over the work annoy people** (Photoshop's Contextual Task Bar). Dock them.
- **Chrome that can't be hidden annoys people** (Premiere's header: an idea thread still open after 4 years).
- **Command search is how people find commands.**
  - Microsoft's study of millions of searches in Office found most were for commands.
  - Blender 2.90's F3 search shows where each command lives.
  - 3ds Max (X), Cinema 4D (Shift+C) and Maya (Find Menu) all have one.

## 3. Patina 0.45 through a new user's eyes

What a Blender or Maya user met on the first start (the screenshots are in the session's notes):

1. **The developer console was in front.**
   - The bottom panel opened on the Console: OpenGL info, "Command server listening on…" and a JSON command line.
   - Materials, the most important panel in a texture painter, was a tab behind it.
2. **Seven tabs at the lower right, their names cut off** ("Blend…", "Histo…", "Textu…", "Prop…", "Retop…").
3. **Unrelated things in one panel.** The Texture Set panel held six: resolution, texture sets, the base material, the
   bakes, the lighting and the mesh stats.
4. **Baking in two places**: the Bake menu and the Texture Set panel, the latter hidden behind Properties.
5. **The bar over the 3D view ignored the tool.** Brush size, hardness, flow, opacity, spacing and projection showed for
   Select, Seams, Fill, Pick and Note too.
6. **Menus by technical area, not by the order of the work.** The UV menu had over 30 items. View held "ImGui demo", a
   developer's window.
7. **No real empty state.** A sphere with an iron paint material was already loaded behind the welcome window.
8. **Unsaved work could be lost.** Only Quit asked; New, Open, Open Recent, the welcome buttons and drag and drop
   replaced the project silently.
9. **The first start offered Patina or Blender keys, but no Maya navigation.** It was there, under View › Navigation.
10. **Nothing said what comes next.** A newcomer had no way to tell that the usual order is model, UVs, bake, paint,
    export, or where each of those lives.

What was already good: drag and drop everywhere, smart materials, the command palette (Search everything), right-click
menus, pies, rebindable keys with Blender and Maya navigation, the bridge, and baked maps as layers.

## 4. What 0.46 does

**The Guided layout** (new installs start in it; feature `guided_layout`). The work in five steps along the top:
**1 Model, 2 UVs, 3 Bake, 4 Paint, 5 Export**. Each is a screen of its own with what that step needs.

| Step | The screen | What it checks |
|---|---|---|
| Model | The view, and: open a file or the sample, a shape, recent files, how to send from Blender. With a model: what it is made of, the mesh check with a fix button, the texture size and texture sets, more mesh tools. | A model is open; the mesh check's problems. |
| UVs | The view beside the UV editor, the UV tools (Select, Seams, Island brush), and: unwrap automatically or along the seams, pack, keep the paint, the checker pattern. | Islands that overlap, faces with no UV area, no UVs: each with its fix. |
| Bake | The view, and: ambient occlusion, curvature, thickness with what each is for, quality, Bake, show a map, from a high-poly model. | Missing, partial or out-of-date bakes. |
| Paint | The view with the paint tools and symmetry, the library shelf (materials, smart materials, smart masks, brushes) under it, the layers at the right with Brush, Colors, History and Scene (base material, lighting) tabs. A "Start here" note while nothing is painted. | Something is painted. |
| Export | The view, and: send the textures to Blender (or send the model as a new object), the texture files (for which engine, size, folder), every export option, save the project, the model with its textures, a render. | Exported. |

How it follows the research:

- **The steps are layouts over one project.** The same panels, tools and commands, and the switch costs nothing.
  - **Guided | Full** sit at the right of the menu bar.
  - The menus and **Search everything** (now also on F3, and on Ctrl+K with the Blender keys) work in
    every step.
  - A tool chosen from another step, by its key, a menu, a pie or the palette, takes you to that step. No tool is ever
    chosen but out of sight.
  - Retopology has its own tools and panel, so it opens the Full layout.
- **Every step is clickable at any time.** Each shows a mark: done, needs a look, or not yet, with why on hover. Its
  last button names the next step ("Next: bake the mesh maps").
- **Two levels at most.** Each step shows the essentials and a "more" button to the full options: every export option,
  details of the mesh check, more mesh tools.
- **Checks instead of tutorials.** Each check comes with the button that fixes it. Hints sit in the panel, never
  floating over the model.
- **In Model, Bake and Export the 3D view only looks.** A left drag turns it, and no tool acts.
- **The first start asks three things**: You come from (Blender / Maya / something else), Your textures go to (Blender,
  glTF, Unreal, Unity) and the layout. In the Guided layout the welcome window appears only that first time; the Model
  step does its job after that.

**For everyone, Full layout included:**

- **Unsaved work is never replaced without asking.**
- **The brush explains itself** when a stroke would paint nothing.
- **The bar over the 3D view only shows the brush for tools that use one.**
- **A fresh Full layout:**
  - Materials is in front at the bottom and the Console is last.
  - History and Notes moved to the bottom too, so the tabs at the right have room for their names.
  - "ImGui demo" is gone from the View menu.
  - Randall's own layout is untouched; *View › Reset layout* gives the new default.

**For agents:** the `layout` command reports the layout and every step's state (done, look, todo, and why), and switches
the layout or the step.

## 5. Done in 0.47, and still open

Done in 0.47.0 (Randall: "all four of those things ... should be done"), with the steps' colors he asked for:

- **The steps' colors.**
  - The step bar is folder tabs, each in its step's color. The chosen tab is raised and joined to the strip under
    them, and the window takes a tint of its color.
  - In the Full layout the same colors mark the tool strip's groups, the panels' tabs and the menus. The menus now
    follow the steps' order.
- **Export preview.** The Export Textures window and the Export step list every file before anything is written: its
  name, what its channels hold, its bits, sRGB or Non-Color. The same goes for agents (`export_textures dry_run`).
- **The UV menu in groups**: Unwrap, Islands, Mirrored Halves, Pins, Selection, Seams. It went from over 30 items to 13.
- **A first start paints a color**, a red, instead of the Iron Raw material.
- **The sample's 58 faces "with no UV area" were Patina's mistake, not the sample's.**
  - They are tiny faces in a dense part of the layout.
  - The check summed their area on the corners' own coordinates in single precision, and the rounding lost it. It now
    measures each face from its first corner, so the sample (and any model with tiny faces) checks clean.

Still open:

1. **"Show me where" in Search.** Each result could name its step and menu path, and right-click could assign a key,
   as Blender's F3 does.
2. **Toolbag's baker** is the one everyone praises: a live preview and a cage painted in the view, for the high-poly
   bake.
3. **A compact UI on small screens**, sized from the display's DPI (Painter's 1080p laptop complaints).
4. **A Space-hold pie like Maya's hotbox**, for Maya users.
5. **Find out how it is used.** With no telemetry, ask the first users two things: where they switched to Full, and
   where they got stuck.

## Sources

Substance 3D Painter
- https://community.adobe.com/questions-59/udim-slow-627748
- https://steamcommunity.com/profiles/76561198056937142/recommended/1775390/
- https://community.adobe.com/t5/substance-3d-painter-discussions/painting-across-materials/m-p/13654691
- https://community.adobe.com/bug-reports-58/the-emission-channel-is-ineffective-in-openpbr-material-mode-1633785
- https://www.versluis.com/2022/10/how-to-flip-imported-normal-maps-in-substance-painter/
- https://community.adobe.com/bug-reports-58/output-maps-wrong-default-bit-depth-despite-template-1186663
- https://blenderartists.org/t/solution-substance-painter-to-blender-normal-map-issue/1344387
- https://community.adobe.com/t5/substance-3d-painter-discussions/not-able-to-paint-or-fill/td-p/13274510
- https://community.adobe.com/bug-reports-58/painting-error-can-only-paint-on-some-materials-and-not-others-1186643
- https://steamcommunity.com/profiles/76561198140701807/recommended/1454900/
- https://community.adobe.com/bug-reports-58/adobe-substance-painter-update-unnecessarily-complicated-zooming-1186686
- https://community.adobe.com/feature-requests-60/petition-to-add-zbrush-navigation-mod-to-substance-3d-painter-1185907
- https://community.adobe.com/feature-requests-60/compact-ui-design-for-smaller-screen-tablets-1185931
- https://experienceleague.adobe.com/en/docs/substance-3d-painter/using/baking/how-to-bake-mesh-maps
- https://experienceleague.adobe.com/en/docs/substance-3d-painter/using/getting-started/project-creation
- https://community.adobe.com/t5/substance-3d-painter-discussions/how-to-import-edited-mesh-with-same-uv-without-breaking-textures/m-p/14195299

Other painters
- 3DCoat: https://3dcoat.com/forum/index.php?/topic/29818-3dcoat-2026-ui-suggestions/ ,
  https://blenderartists.org/t/3d-coat-as-a-companion-to-blender/1353279?page=19 ,
  https://steamcommunity.com/app/100980/discussions/0/1698294337775571969
- Mari: https://www.foundry.com/insights/texture-artist-mari , https://www.foundry.com/news-awards/new-release-mari
- ArmorPaint: https://blenderartists.org/t/review-of-armor-paint-its-a-trick/1155631 ,
  https://forums.armory3d.org/t/baking-normal-maps/5862 , https://forums.armorpaint.org/t/first-impresions-with-armor-paint/85
- Blender: https://blenderartists.org/t/the-big-texture-paint-thread/1553031 , https://developer.blender.org/T42330 ,
  https://devtalk.blender.org/t/blenders-texture-baking-design-is-extremely-dangerous/12193 ,
  https://extensions.blender.org/add-ons/ucupaint/reviews/?score=1
- Marmoset Toolbag: https://docs.marmoset.co/?p=1357 , https://80.lv/articles/using-marmoset-toolbag-4-as-the-main-texturing-tool
- InstaMAT: https://digitalproduction.com/2026/01/28/instamat-test-painting-procedurals-and-pasta
- Quixel Mixer: https://blenderartists.org/t/quixel-released-mixer-2020/1209685 ,
  https://www.cgchannel.com/2026/02/quixel-mixer-has-been-discontinued-get-the-final-version-for-free/

Simple modes, disclosure and search
- https://www.premiumbeat.com/blog/davinci-resolve-16-cut-page-review/ , https://mixinglight.com/learning-cut-page-speed-editor-davinci-resolve
- https://affinity.help/photo2/en-US.lproj/pages/Introduction/about_Personas.html
- https://docs.blender.org/manual/en/latest/interface/window_system/workspaces.html , https://projects.blender.org/blender/blender/issues/61002
- https://helpx.adobe.com/ro/photoshop-elements/getting-started/editor_workspace.html
- https://community.adobe.com/t5/photoshop-beta-discussions/contextual-task-bars-now-in-photoshop-beta/m-p/13820058
- https://community.adobe.com/t5/idea-premiere-pro/option-to-remove-new-header-bar-(22.3)-from-window/idi-p/13515916
- https://learn.foundry.com/mari/4.5/Content/release_notes/Mari_4_series.html
- https://learn.microsoft.com/en-us/archive/blogs/jensenh/combating-the-perception-of-bloat-why-the-ui-part-3
- https://link.springer.com/chapter/10.1007/978-3-540-74796-3_59 , https://researcher.watson.ibm.com/publications/training-wheels-in-a-user-interface
- https://www.nngroup.com/articles/progressive-disclosure/ , https://www.nngroup.com/articles/wizards/ ,
  https://www.nngroup.com/articles/empty-state-interface-design/ , https://www.nngroup.com/articles/mobile-tutorials/
- https://www.microsoft.com/en-us/research/publication/characterizing-search-productivity-software/
- https://developer.blender.org/docs/release_notes/2.90/user_interface/
- https://docs.blender.org/manual/en/latest/getting_started/configuration/defaults.html
- https://help.autodesk.com/cloudhelp/2023/ENU/Maya-Basics/files/GUID-941480C1-9BDA-4DB3-8EA8-113A0D6FAF1F.htm
