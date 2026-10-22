# Retopology in Patina: working plan

Randall, 2026-10-15: "a CozyBlanket Pro style retopology tool, borrowing from our Island Painter,
allowing the user to define mesh density per m² and selectively increasing density, retopologizing
in quads along the way. This is not a Quad Remesher like ZRemesh or voxel retopology - those are
too rudimentary." Sample to work from: `~/Documents/mai_clothing_workflow/Anime Girl.blend` (one
Tripo mesh, 1.93 million triangles, 0.98 m tall; a copy exported to
`~/patina-work/retopo/anime_girl_hi.glb`). Plain words, British spelling, as in the manual.

Randall, after 0.13: "we need to be a lot higher quality retopology than that"; about 98% of the
vertices four-edged; "I'm expecting the user to set the locations edge flow should change, and that the
actual painting of individual edges isn't expected, but allowing the user to easily scale resolution
within a field ... scalable zones that define the direction of flow and each zone has its own parameter
for resolution; the expectation is not that the whole process is automated, but that it is responsive
to the user's input, and the more input there is the more detailed the outcome becomes."

## 0.22: lines that stay out of the way

Randall, 2026-10-16: "all lines are basically visible through the mesh at all times, which is very
distracting", and line ends snapping in tight folds (the crotch, the mouth, fingers and toes).

- **Why they showed through**: the lines were pulled toward the eye by a share of the depth range
  (0.003 in clip space); with a perspective camera that share is a long way at the back of the model, so
  most of the far side showed. Now they are pulled by a distance (0.4% of the camera's distance, as the
  quads' edges are) and drawn by the depth test: **Lines behind the model** shown (depth test off),
  darkened (the far parts again with the depth test reversed, at 45% brightness and alpha, thinner), or
  hidden. Kept in session.json (`retopo_lines_behind`); `retopo_view` `lines_behind`.
- **Snap line ends** (on by default, `retopo_snap`, session.json `retopo_snap_ends`): off, a new line's
  ends stay where they are drawn. Closing a line on its own start still works.
- **Confirmed lines** (`Stroke.confirmed`, saved with the lines: bit 2 of the loop flag): drawn a darker
  brown, never snapped to, and an unconfirmed line wins a Ctrl+click over a confirmed one. Confirm lines /
  Unconfirm all in the window, Ctrl+Shift+click one (with its mirror images), `retopo_confirm_lines`. One
  undo step that changes neither the zones nor the quads (no new build).
- The window's Make quads row (with the live preview and the count) stays at its foot; the rest scrolls.
- Found on the way: the live preview only started when the window had asked for it within the last
  second, and a frame can take a second (a heavy model; a virtual display whose session was locked):
  now three seconds. GUI tests: start the virtual KWin with `--no-lockscreen` (it locks itself, and a
  locked session gives the window one frame a second).

## 0.23: a change remakes only the quads near it

Randall, 2026-10-17: "I don't think the process of converting from preview to actual quads is being maintained
properly, as the mesh is getting jumbled in zones where the preview looked very clean."

What it was: Make quads keeps an up-to-date preview exactly (checked in the real window on Mai, two rounds: the same
file byte for byte). But every build made all the quads again, and QuadriFlow's result moves everywhere when anything
changes: one more line on the Suzanne layout moved 75 to 80% of the vertices more than 12 quads away from it, and most
poles there were new. A zone that looked clean in one preview could come out jumbled in the next with nothing in it
changed.

What 0.23 does (core/RetopoLocal, flowQuadsLocal in RetopoQuads.cpp): each Result keeps what it was made from
(Result::builtFrom: strokes, density, zones' resolutions, patches, settings). The next build compares: the lines and
strokes drawn or taken away, the density painted, the faces whose resolution or patch changed, and (for a closed line
under 12 quads across) the inside of the loop. A new build is made as before; its quads within a reach of the change
are taken, the last quads beyond it kept exactly, and one row of faces laid between: a search over both outlines (one
edge each: a quad; two of one round a vertex of the other: a corner quad; triangles only as a last resort) that costs
each vertex left with other than four edges and each badly shaped quad. The hole in the kept quads is the new patch
grown by two thirds of a quad, so the row is about a quad wide all round. Edge lines crossing the row run on across it
(their crossing points are fixed rungs). The row is tried at 2.25, 3, 4 and 5 quads from the change and the one with
the fewest poles kept; pairs of poles along it are taken out (cleanJoinPoles, three rings round it); with Fewer poles,
the joined quads are solved again with every run through the kept quads held at its count (else a strip taken out to
cancel two poles reaches far off), and the kept vertices put back exactly. Nothing changed: the last quads as they are
(not mirrored again). Settings changed (density, rings, Fewer poles, mirror): all made again. Remake all (and
retopo_build fresh) makes everything again; Keep quads away from changes (retopo_keep_away, saved as keep_away) off
gives the old behavior.

Numbers (Suzanne layout of 8 lines added one at a time, 1,200 quads/m², Fewer poles on): every vertex more than six
quads from each new line stays exactly where it was (was 75 to 80% moved); poles after the 8 lines 184 (95.0%)
against 111 (96.0%) remaking everything each time. The cost is the row (about 15 to 30 poles before tidying, 3 to 15
pairs taken out) and keeping what the new build made near the line even where it is poor (line 4: the fresh build
there has 151 poles in all). Mai with Mirror X and Randall's four lines: 91 poles, 97.0% after the four, against 96
poles, 96.7% made at once. A ball with two circles: 56 poles against 60 made at once. Tests:
retopo.a_change_remakes_only_the_quads_near_it_with_a_row_between (flat grids at 0, 0.15, 0.4 radians: 14 to 24 row
poles, every far vertex where it was), retopo.a_line_drawn_later_leaves_the_quads_away_from_it_as_they_were,
tests/api/test_retopo_local.py.

Tried and left: starting QuadriFlow from the last field (a warm start: the field away from the change kept exactly,
only 10% of the field vertices worked out again) still moved 80% of the vertices far away, because QuadriFlow's
integer solve and the positions after it are global (raw layouts far away agreed in 85% of their edges, shifted by
0.4 of a quad; patch in scratchpad prev25/warm_start.patch). A row that only steps 3 to 1 (the joins' stepRow) can't
take a hole's corners: 45 poles on one line; the corner quads brought it to 14 on aligned grids. Fewer poles over the
whole joined quads canceled more (144 poles after 8 lines) but moved quads far off and dropped 10 to 50 strips of
quads a time.

Next: a row with a second loop of new vertices where the outlines' corners don't match (a notch needs two rows), and
trying the new build's tries round the change for the best quads there.

## 0.22.4: flow strokes as loops by default; mirrored creases and small holes

Randall, 2026-10-17: Strokes make edge loops should be on by default (yes), and Mai's UE5 model has her hair as a
separate mesh. Now on by default (`RetopoState.flowLoops = true`). The .retopo header has `format: 2`; files saved
before it (0.22.3's off was only the default) load with it on.

Tested Mirror X on Mai's UE5 body with two hair styles as separate pieces (Fubuki, the side-swept ponytail): the body
mirrors exactly (median image distance 0), and pieces of hair not the same on both sides keep their own quads. Found
on the way, all from 0.22.3 or older:

- **Faces flat on the middle, doubled.** The walls of a crease along the middle (the crotch, the buttocks' cleft, the
  cleavage) come within a third of a quad of the plane and were snapped onto it; a face with every corner on the
  plane was kept and mirrored onto itself, turned over: 9 to 29 doubled faces in every mirrored Mai, read as folds.
  Such faces are now left out; the faces round them meet their images along their edges, so the crease is bridged.
  Mai's body (lines as in Randall's shot): folded faces 22 -> 0, 96.9% -> 97.5% four-edged, 86 -> 68 poles (this fix
  alone; with the line change below too, 97.1%: the build took a second pass and made a third more quads). Test:
  two balls joined at a narrow neck (`makePeanut`).
- **A line near the middle at a slant.** A breast outline by the cleavage came within half a quad of the middle at a
  few points only: those were moved onto the middle's loop and the rest left a sliver beside it, and where the
  outline and its image met the middle a star of 14 edges came out. Now the whole run of a line's points within a
  quad of the middle around a point within half a quad goes onto it: the line shares the middle's loop there and
  leaves it across it. Breast case (Mirror X, zones at x4): 95.4% -> 96.3%, folded 8 -> 2, the star gone.
  Suzanne bench with Mirror X: 96.73% / 1,449 poles -> 96.75% / 1,410 (no mirror: unchanged, 96.37% / 1,576).
- **Small holes left open beside hair.** QuadriFlow's holes of three or four edges were filled only when the whole
  group's border was simple; hair (strands touching) never is, so a triangle hole in Mai's buttocks stayed open.
  `smallHoles` finds them anyway (and never takes a lone face's own border for a hole); one up to twice a quad's
  perimeter is filled too when none of its corners is by an open edge of the surface.
- The middle's sections over pieces not the same on both sides (hair worn to one side) aren't laid as lines: those
  pieces keep their quads as made, so a loop down their middle only cost them.

Left: where the two feet's toes touch at the middle (Mai's T pose) the mirrored halves share an edge four faces use.

## 0.22.3: mirrored quads; flow strokes explained, and as loops

Randall, 2026-10-17, with a screenshot of Mai's chest (Mirror X on): "there's clearly a lack of symmetry between
X+ and X-", and "I'm trying to understand how Flow lines actually influence the topology because they seem like they
do very little" (two cyan rings round each nipple, inside a zone between the breast's outline and a nipple ring).

**Symmetry.** Mirror X only mirrored what was drawn; the quads were made on the whole model, and QuadriFlow isn't
symmetric. Now (`flowQuadsMirrored` in RetopoQuads.cpp, when `FlowOptions.mirrorAxes` is set):
- First a check of the model: its vertices sampled (40k), each piece's share whose mirror image is within half a quad
  of the surface. When no piece holding a fifth of the samples is symmetric (98%), the quads are made on the whole
  model at once, the differing spots marked, and the window says so (the anime girl: her hair, welded to the body,
  hangs to one side; 87% of the model mirrors within 2 mm).
- The middle (`middleSection` in RetopoMirror: every triangle the plane crosses, chained) laid as edge lines, so
  QuadriFlow's edges run along it exactly; points of the user's lines within half a quad of it moved onto it (else a
  strip too narrow for a row of quads was left between, and the quads crossed the middle: Mai's cleavage, Suzanne's
  brow); their pieces along it taken out. The zones either side of it are kept from the ring and grid-disc rules
  (`noRingsAt`): a ball cut in two looked like two discs and got a coarse grid.
- The whole model made with those lines; then (`mirrorHalf`) the kept side's faces (the side with more line length;
  the bigger surface without lines) kept, vertices within a third of a quad of the plane put on it, faces still
  across it (a crease narrower than a quad) cut along it, and mirrored; the images put on the surface (closest point)
  and checked. Per piece of the quads: a piece whose images miss the model is kept as made (and said); a piece wholly
  on the far side whose images land goes.
- Suzanne bench (15 layouts) with Mirror X: 96.73% regular, 1,449 poles, no fallbacks, no holes at the middle (no
  Mirror: 96.37%, 1,576). Randall's Suzanne lines: 96.5% / 57 poles (95.8% / 62). Folded faces 45 (18): each counts
  twice, and the side kept is the busier one; none at the middle. Mai obj (breast outline + nipple ring lines):
  96.9%, 86 poles; mirror images within 0.8 mm (the surface's triangles aren't mirror images of each other).
- Left: a helix on an unlined neck becomes a chevron when mirrored (a neck line fixes both); a single piece that is
  partly asymmetric is made whole (mirroring part of a piece would need the seam stitched).

**Flow strokes.** Measured on Mai's belly (a diagonal stroke): quad edges within half a quad of it 38 degrees off it
without the stroke, 7 with; 3 to 6 quads away 28 -> 19. One stroke turns the whole torso. So strokes work, but as a
direction only: the quads keep their size and don't form a loop along the stroke, so rings round a nipple came out
as spirals with poles where the rings grow; and ring zones and grid discs are laid out from their lines alone. Drawn
as edge strokes (`Stroke.edge`, the same as a line in the build but no zone of the engine's own) the rings became
loops. Now an option, **Strokes make edge loops** (`RetopoState.flowLoops`, saved as `flow_loops`, the command
`retopo_flow_loops`; off by default).

Found on the way: on a coarse model (Mai obj, faces about a centimeter) zone lines closer than a face or two break
the engine's zones (per model face) into patches, so a zone's resolution only reaches some of it; the build's own
zones (on the refined working surface) are right. Not fixed yet.

**Widget IDs.** The Retopology window's Show: Density radio had the Density tool button's ID (Dear ImGui's red
"conflicting ID" box on hover); the Export window's Export combo its button's. With `PATINA_UI_ID_CHECK` set,
Dear ImGui's test-engine item hooks (built in, idle unless set) count every widget each frame; `ui_id_conflicts`
lists clashes; tests/gui/test_ui_ids.py opens the tools, tabs, menus and every window and expects none.

## 0.22.2: drawing gets the whole machine

Randall, 2026-10-16: "make sure everything is properly gpu accelerated", and pause the rest while drawing
for the smoothest lines. Measured with the new `PATINA_FRAME_STATS=<file>` (CPU per part of the frame, GPU
of the viewport and the frame by timestamp queries), anime_girl_hi.glb (about a million triangles) with the
face's lines and quads, a stroke across the face, RTX 5090, 60 Hz virtual display:

- Before: 60 fps throughout (vsync); CPU about 5 ms a frame, the biggest part the UV fingerprint (every UV
  hashed each frame: 2.5 ms at 24 MB of UVs), the viewport 0.57 ms on the GPU.
- Now, while a line, flow stroke or density stroke is drawn (Engine::drawingStroke): the UV fingerprint is
  held (uvHashHold), the UV Editor and the 3D view reuse their last pictures unless the view or what it
  shows changed (a key of the camera, the size, the retopology keys, the undo position), a live preview
  being made stops and none starts until the stroke ends. CPU 2.7 ms a frame, viewport GPU 0.
- Left: letting go of a line on that model costs one frame of about 180 ms (the zones found again over a
  million faces, and their colors sent to the GPU).
- GPU or not: the viewport, painting, layer compositing, masks, mesh-map bakes, thumbnails and the UV view
  are shaders; retopology, the high-poly bake's rays and UV unwrapping and packing run on the processor
  (threads). Windows: NvOptimusEnablement / AmdPowerXpressRequestHighPerformance exported, so laptops
  with two GPUs give Patina the fast one.

## 0.22.1: Shift+click round what faces you (breasts, faces); noses and lips open

Randall, 2026-10-16: "we don't have any way to do the circumference line by shift+click on things like
breasts, noses, lips, or faces". Shift+click (and its preview) now tries round the part first (ringAround,
a tube), and where there is none the cut facing you (facingLoop: planeSection through the click, its
normal the camera's forward axis, the loop through the click; mode 2). Shift+wheel switches between them;
a label by the pointer says which. `retopo_ring_line` takes `mode` (auto, around, facing) and `direction`.

- Parallel to the screen, not along the line of sight: on a body framed whole the line of sight to a breast
  is about 8 degrees off the axis, and the tilted plane ran down the torso (2.7 m loops on Mai).
- Breasts (a ring at the depth clicked) and the face (in front of the ears, under the jaw) come out clean
  on Mai. Noses and lips don't: at their depth the plane also cuts the forehead and cheeks.
- Tried for noses and lips (prototypes in Python, not in Patina): (a) height above a quadric fitted to a
  band round the click, level set round the click: the band crosses eyes and mouth, the nose's bridge runs
  on into the forehead (never enclosed); (b) height above the model smoothed at the bump's scale (implicit
  cotan smoothing; the scale where h/sigma peaks: nose 1-4 cm, lips 1-2 cm, breast 7 cm), min cut between
  the top and a rim weighted exp(alpha h): unstable, loops wander over the face; (c) min cut weighted by
  the bend across edges (creases cheap): promising round the nostrils but the cut's boundary came out in
  pieces. Next: (c) with a proper loop extraction and smoothing, the window from the scale estimate.

## 0.21: quad layouts and one count solve (Fewer poles in 0.21.0)

Randall, 2026-10-16: "Patch layout rebuild please." Two uses of one machine. Shown the three side by side
(the flow, the layout, the flow solved again), Randall picked the flow solved again as closest to what he
expects; it shipped in 0.21.0 as **Fewer poles** (on by default). The layout method stays API-only:

- **Method "layout"** (`retopo_build` `{"method": "layout"}`; core/RetopoQuadLayout): one cross field over
  the working surface (QuadriFlow's orientation step, held along the drawn lines' own way and the open
  edges); singular triangles found by matching the field round each (pairs of a three and a five closer
  than a quad cancel; one touching a line is put on the line); each singularity's separatrices (3 or 5)
  from a smoothed ring of samples; separatrices that face each other along the field joined; the rest a
  motorcycle graph: every trace cut into the working triangles as it goes (edge splits, exact), stopping
  where it meets the layout; loose ends carried on; corners inwards and singularities' double sectors get
  a trace into the patch and the patches are found again; two patches beside each other whose corners
  together come to four (or a strip thinner than 0.4 of a quad) made one. Then the counts.
- **The counts, all at once** (`quadsFromLayout`): the arcs (runs of layout edges between nodes) are
  the unknowns of one min-deviation flow (libSatsuma, Bi-MDF, vendored, MIT): a grid patch balances its
  opposite sides, a three-, five- or six-cornered patch takes the midpoint pattern (each side split, a
  spoke to the middle: one pole), other patches get four corners chosen; arcs may come to nothing
  (strips fold away, vertices merged). Each patch filled through a harmonic map of its triangles onto a
  rectangle or regular polygon. Small holes closed (up to 32 edges; capped).
- **The flow's quads solved again** (`PATINA_REQUANT=k`, FlowOptions.requantize; `layoutFromQuads`): the
  flow's quad mesh as a layout (its poles' separatrices along its edges, exact corners: one quad at a
  vertex); each run aimed at its count now, a run of two edges or less between a three- and a five-edged
  pole drawn toward nothing (weight k) so the pair cancels; patches whose counts don't change keep their
  quads; the triangles' runs held. Kept only when it has fewer poles and no more folded faces.
- **Fewer poles, 0.21.0** (`FlowOptions.requantize`, the window's checkbox, `retopo_fewer_poles`; k = 6):
  the check above runs on the raw quads, but the smoothing and fold repair after it can still leave more
  folds (3 of the 15 layouts). So when the re-solve is kept, the build is made again without it (the
  working surface and the zones' quads come from the cache: about 0.4 s on Suzanne) and the finished
  quads compared: the re-solved ones are kept only with fewer poles, no more folded faces (by the faces
  round them), at least nine tenths of the quads and no more of the surface left out. Holding the rings'
  vertices through the re-solve was tried: worse (30 folds against 26).

Fewer poles in 0.21.0, the 15 Suzanne layouts: 96.23% to 96.37% four-edged, poles 1,655 to 1,576, folded
faces 20 to 18; kept on 4 of 15 (Randall's lines 70 to 62 poles, 1 to 0 folded), never worse on any. The
girl, Mai, four garments and the ball come out exactly as in 0.20.8 (nothing to cancel that passes the
checks). Builds take up to a second longer where it is kept.

Numbers (Suzanne, 600 per m², Randall's lines: flow 1,552 quads, 70 poles, 95.4%):

| | Quads | Poles | Four-edged | Folded | Time |
| --- | --- | --- | --- | --- | --- |
| Layout | 1,493 | 77 | 94.7% | 41 | 0.8 s |
| Flow, solved again (k = 6) | 1,502 | 62 | 95.8% | 0 | 1.5 s |

The layout's figures count only the quads it makes: it leaves 3 patches open on Randall's lines (rings
and patches with more than one boundary aren't filled yet; about 4% of the surface uncovered) and stretches
quads where an arc's count comes out low (324 edges over twice the size asked for). A regression from the
re-solve's hole capping (holes up to 32 edges capped in the layout method too: 77 to 110 poles) was found
while taking screenshots and fixed (capping is for the re-solve only).

Nothing drawn (flow 1,619 / 68 / 95.8%): layout 1,448 / 54 / 96.2%. The ball: 8-10 poles (its eight).
The 15 Suzanne layouts: layout 96.68%, 1,668 poles, ~1.5 s each but 717 folded faces (flow 96.23%, 1,655,
20); flow solved again without the guard 96.79%, 1,413 poles, 27 folded (girl: 14% fewer quads and 4x the
folds), with it 96.39%, 1,563 poles, 26 folded (kept in 8 of 15; the girl and Mai left as they were).

What's in the way, and what's next:
- The layout's folds come from patches that aren't four-sided (patterns, corners chosen): corners are
  found from the bend at each node (over half a quad; the drawn line's own way along lines); counting them
  in the field's terms made the solve fail with lines. QuadWild chooses which traces to keep (a patch
  layout optimization); here they are repaired in rounds. Next: corners from the field index along whole
  arcs, and a choice of traces like QuadWild's (without its code: GPL).
- The solve: libSatsuma's first stage only searches near its first guess's parities and calls a layout
  whose counts some corner forces to nothing infeasible; it is tried again (patterns' halves of nothing;
  counts rounded to even; each patch's balance allowed off at a high cost, filled with stepped rows). It
  takes no fixed counts (lower = upper), prints to stdout unless told (two lines commented out), and its
  .bimdf dump writes hex floats its own reader can't read.
- Fewer poles: a re-filled patch (counts changed) can fold on tight shapes (the girl's back, 3 of the 15
  layouts), so it is turned down there; re-filling only along the canceled pairs' strips would let more
  builds keep it. A pair made by a turned edge (two threes and two fives in a diamond) can't be canceled by
  counts at all (no strip comes or goes): that needs an edge turned back.

## 0.14: quads along the shape, steered by zones

- **The quads follow the shape by themselves.** QuadriFlow (vendored in `third_party/quadriflow`, with
  the changes in Blender's copy, Eigen and LEMON) makes them along a smooth field over the whole model.
  Six fields run at once (three seeds × three amounts of normal smoothing, about a quad's worth); the
  one kept has the fewest poles, a count near the one asked for, and no part where QuadriFlow failed
  (quads off the surface, or parts of the surface far from every vertex). Pairs of three- and
  five-edge poles are collapsed or turned away afterward, tiny holes filled. The count is measured
  and the run repeated (up to three times) until it is within 10% of area × quads per m².
- **Zone lines are exact edge loops.** Each line is laid into the working triangles first (a path along
  their edges that keeps close to the line, Dijkstra; its vertices moved onto the line along the
  surface unless a triangle would fold) and its edges marked sharp for QuadriFlow, which follows them
  only roughly; so the line is laid again into the quads afterward, the same way, and the final
  smoothing slides the vertices on it along it only. Since 0.14.1 lines are free polylines on the
  surface (every pointer sample raycast, like a grease pencil; ends snap onto other lines, a stroke
  ended at its start closes). Zones are the faces between them: the faces a line crosses are found by
  walking from point to point (halving the step until the faces are neighbors, so a closed line always
  cuts), the rest flood-filled, and the crossed faces given to a neighboring zone; only when lines or
  painted patches change (linesVersion). For the quads, a line's loose end past the last line it touches
  is cut off when shorter than a quad, and a stray line shorter than a quad dropped.
- **Zones with a resolution of their own.** Closed lines (and painted patches) split the model into
  zones. Zones at the same resolution are made together; each other group is made on its own
  triangles plus a margin of about two of its quads past its lines (so its lines are inside the mesh
  QuadriFlow sees), and only the quads on its side are kept (by the nearest of its own triangles;
  quads meeting at a corner only are trimmed to one fan). Where two groups meet, the side with fewer
  vertices along the line (or the other, where that side is too narrow) loses its row of quads along
  the line, and the gap is filled with a row that steps from one count to the other: plain quads,
  three-to-one steps (four quads, two three-edge vertices, spaced apart where they can be), one
  triangle and a quad for an odd difference; steps are placed by where each side's vertices are
  across from each other. Then that row is smoothed more. When borders meet where three zones of
  different resolution meet (or at an open edge), or a zone is narrower than its quads, the zones are
  made together instead, with their resolutions in the size field, and the window says so.
- **Flow strokes** hold the field's direction in a band about a quad wide around them.
- **The density brush** goes into QuadriFlow's size field, with its contrast doubled (QuadriFlow follows
  a size field only partly: its integer step removes the poles a change of size needs). Gentle; zones
  are for clear steps.
- **Big or messy models** (over 300,000 triangles, or more than 60 per quad) are first wrapped in a
  voxel shell: the distance field near the surface (coarse cells first), the outside only (a flood
  from the grid's edge, so closed models don't get a second shell inside), surface nets, vertices put
  on the offset surface exactly and Taubin-smoothed. Non-manifold edges are cut, loose specks under
  about four quads dropped. The quads are made on the shell and projected back onto the model.

Numbers (Linux, 24 threads, release build):

| Model | Settings | Quads | Four-edged vertices | Time |
| --- | --- | --- | --- | --- |
| Ball (test) | 400 per m² | 1,241 | 99.4% | 1.3 s |
| Torus (test) | 400 per m² | 5,573 | 99.7% | 2.7 s |
| Ball (test) | zone line round it, cap at ×4 | 2,302 (1,795 and 378 per m², asked 1,600 and 400) | 96.1% | 1.9 s |
| Subdivided Suzanne | 600 per m², nothing drawn | 1,603 | 95.9% | 1.0 s |
| Subdivided Suzanne | five zone lines as Randall drew them | 1,524 | 94.4% | 1.0 s |
| Subdivided Suzanne | 1,000 per m², face ×2, snout ×3 | 3,768 | 93.1% | 3.8 s |
| Anime girl (1.93 M triangles) | 4,000 per m², nothing drawn | 2,443 | 96.7% | 1.5 s |
| Anime girl | wrist lines, hands ×3 | 2,365 | 94.8% | 5.7 s |
| Anime girl | 10,000 per m², hands ×3 | 5,968 | 96.3% | 15 s |

On a closed, ball-like piece the three-edge poles outnumber the five-edge ones by eight, so a separate
eyeball or ear always brings its own few; Randall's 98% is reached on smooth parts, and lines and
resolution changes each cost a few poles.

Still to do: zones of different resolution meeting three at a point, zones narrower than their quads
(both fall back to the size field), symmetry of the result, PolyBuild-style editing of single quads,
corners placed by hand.

## 0.19: grids for discs and holes, ring counts planned

- **Discs closed with grids** (`gridFill` in core/RetopoRings). A zone that is a disc and has nothing
  round it but one ring zone or the model's open edge (an eyeball, the middle of an eye's rings, the tip
  of a nose, the inside of the lips; up to 1,500 quads) is mapped harmonically onto a square (cotangent
  weights kept positive, so no fold) whose sides are four stretches of its loop: n, m, n, m edges with
  n + m half the loop's count and n x m its area in quads, corners where the loop turns most. An n x m
  grid is laid back through the map (points found in the square's triangles). All its inner vertices
  have four edges; the four corners are the poles a disc needs. An odd count, or a map that folds, falls
  back to the old cap. A disc beside an ordinary zone stays in that zone's QuadriFlow run (no join).
- **Ring counts planned** (`planRingCounts`). The rings' loops form chains (an eye: the socket's edge,
  the line round the eyeball, the line round the socket). Each loop's quads must be 0.55-1.6 of a quad
  wide; a step costs the same inside a ring or where a loop meets a zone (which has about length / L
  points there), a little less there; loops on the model's edge, discs or other rings take any count. A
  small dynamic program along each chain picks the counts; the eye keeps one count from the socket out.
  Loop lengths are measured on a copy smoothed over about a quad (the loops zigzag along the working
  triangles). A neighbor's loop much denser or sparser than the quads (0.5-1.75 L apart) is no longer
  taken as a ring's own (it folded the rings on a ball).
- **Holes where zones meet closed with grids** (`gridHole`, `gridOrSplit` in core/RetopoJoin). A Coons
  patch from the hole's four sides (chosen as for discs), points snapped to the surface; refused where
  neighboring patch quads turn against each other or a quad turns over when snapped. Otherwise the hole
  is cut across its narrowest neck (points along the cut, so both parts stay even) and each part tried
  again, four cuts deep; ring by ring only when that fails. An odd hole loses a triangle at its sharpest
  corner first. Area: a fan from the middle snapped to the surface (a hole over the bridge of a nose is no
  slit). Holes closed so are no longer marked red.
- **Untidy lines keep their rings** (after 0.18.1): when the zones won't join with rings, only the rings
  that meet other zones along part of a line are made as other zones (two tries), then none; a retry also
  when more than 5% of the joining faces turn over; rings narrower than 0.35 of a quad refused.
- Tried and dropped: soft field constraints along the principal curvature of limbs (both arms twisted
  worse); absolute values on both sides of QuadriFlow's sharp-edge index choice (the girl's strands left
  out); allowing holes over 64 quads round (a nose spanned by stretched quads; dropping the rings there is
  better). Arm twists are fixed by drawing a line round the shoulder: the arm is then a band and gets rings.

Numbers: Suzanne with lines round each eye, socket, the nose and the mouth at 1,200 per m²: 89.4% ->
91.4% four-edged, 2.5 s; twelve jittered copies of those lines: rings kept in three with no fallback,
folded faces 3-16 (were 11-26); two circles round a ball: 98-99%, the cap a grid (test
`a_disc_inside_a_ring_is_closed_with_a_grid`); the girl unchanged (93.2%, nothing left out).

### 0.20.8: folded faces smoothed out

- After the quads are smoothed, faces turned over are found two ways: against the model's surface under
  them, and against the faces sharing an edge with them (on average). Their corners are moved to the middle
  of the vertices round them (on the surface; on a line they slide along it; ring vertices stay), three
  times, then counted again. Each piece of moved vertices is kept only if its faces have fewer folds of one
  kind and no more of the other; the next round reaches a ring of vertices further (six rounds at most).
  Topology is not changed.
- Numbers: the 15 Suzanne layouts 36 -> 20 folded faces (the jittered twelve 34 -> 18), 96.23% four-edged
  and 1,655 poles as before; the girl 5 -> 4; Mai 16 -> 11 faces against the surface. What stays is at
  sharp creases (the corners of the mouth, the crease beside the nose).
- Jitter check (the twelve jittered copies of the Suzanne lines, 0.20.7): rings kept in 9 of 12 (were 0
  of 12 in 0.18). The other three lose their rings for "a hole of 63 edges": the band between an eye's ring
  and the line outside it is about one quad wide on one side, its quads there are dropped as lying along
  the borders, and the ring's loop, shared with that band, is left open beside the face. Tried: closing it
  as a row between its two sides (its outline has notches, so no clean pair of sides), and cutting it up
  from its ends (95.3% instead of 96.4%, 9 folds instead of 7, 35 more poles): making the rings again as
  other zones is better, so that stays. The band there is one row of 32 quads; it is the coarser side
  against the two rings it touches (rings keep their edge), so 14 of its quads are cut for the joining
  rows and the ring's shared loop is left with nothing beside it. Also tried: counting a ring beside a zone
  as finer when thin parts and necks are given away (as the join does): seed 3 still fell back, seeds 7 and
  9 came out at 95.5% and 94.7% (were 97.1% and 97.3%) with more folds. Not kept; in these layouts the
  remade zones give the best scores of the twelve anyway.
- Debug (`PATINA_RETOPO_DEBUG=/path`): the join dumps are numbered per try (`_0_made`, `_0_cut`,
  `_0_joined`), and the log lists the loops the rings share and which borders are joined.

### After 0.20.5: count passes, tried

QuadriFlow's first pass mostly gives 75-90% of the quads asked for, and a second pass is a whole run more
(the girl's x8 head: 2.5-7.6 s per edit). Tried and left: asking for 1.15x at once (edits 3.0-3.6 s, but
95.0-95.4% instead of 95.4-95.8%, and 14% more poles on the Suzanne layouts); half the first pass's tries
at 1.2x (girl fine, Suzanne 20% more poles); three tries more at 1.2x and the best of those near the
count (Suzanne 23% more poles). A heavier count weight in the score changed nothing: every try was short.
Today 0.19.7 and 0.20.5 take about as long on the same edits (20.1 and 21.3 s for four lines). A coarser
working surface: refined to 0.5 of a quad instead of 0.4 (coarse models such as Suzanne) builds 33% faster
(the 15 layouts 49.9 s instead of 74.4 s) at 95.99% instead of 96.20%, 4% more poles: left at 0.4 for now,
an option if speed matters more; decimated to a 2.4th of a quad instead of a third (the girl) was no
quicker and twisted an arm again.

### 0.20.4: a line round a part in one click

- Shift+click with Lines (and `retopo_ring_line`): `ringAround` (core/RetopoThin) takes the part's
  thickness d from a ray in from the point (the face it meets faces away from it, whichever way the
  model's normals point), a first axis from the area-weighted normals within 0.75 d of the part's middle
  (the way of least spread: on a tube, round or flattened, the normals lie across the axis), then cuts the
  model across that axis through the point and takes the axis again from a band of the surface within d/5
  of the cut, until it settles (2 degrees; a sphere took in a chin, or missed a waist's sides). The section
  loop through the point (planeSection) is the line, kept to about 256 points. No line where the first
  look is round every way (ratio of the two least spreads over 0.9: a ball, a head), the band's over 0.35,
  the cut doesn't pass the point or runs round more than 2.5 pi d.
- The girl: upper arm, forearm, thigh, knee, neck, waist, hips, chest, a hand all ringed in 0.04-0.2 s;
  her head refused. An oval ellipsoid's ring matches its perimeter to 0.04% (test
  a_click_on_a_limb_gives_a_line_round_it); the GUI test Shift+clicks the ball's finger.

### After 0.20.2: twisted limbs, measured

- On a cylinder every constant-angle direction field is as smooth as another, so QuadriFlow's field on a
  limb with no line across its top settles at any angle: the rows wind into a helix. On the girl at 9,000
  per m2 every one of the six tries had a twisted limb (arms in some, a leg in others); the field-shear
  term picks the least twisted. A soft hold of each working vertex along the principal curvature
  direction where the surface is tube-like (curvature fit from normals, kmax x L > 0.4, kmin < 0.1 kmax,
  weight 0.6) kept arms and legs straight (lean 4-9 degrees) but cost poles everywhere (Suzanne layouts
  96.2% to 95.0%, the girl's body loops 95.1% to 93.0%, trousers 98.2% to 97.4%): not kept. A line round
  the top of the arm (or thigh) makes the limb a band of rings: arms clean at 4,000 and 9,000 per m2 with
  95.0% and 96.8%. The manual says so.

### 0.20.2: small caps beside the face go with it

- A small disc (2-24 of its quads: the middle of an eye, the tip of a nose) was always a cap of its own,
  rings shrinking in, even when the ring round it had gone with the face (rings only where they pay): on
  the girl at x8 the left eye's middle had 12 points round it against the face's 33, the join failed to a
  hole of 43 edges between the eyes and the nose, no grid fitted it, and it was filled ring by ring, a web
  of stretched quads. classifyDiscs now also makes a small cap beside a zone QuadriFlow makes at about
  its own resolution (within 20%) part of that zone, as big discs were. Caps inside rings stay caps.
- Girl (thin parts, neck line, head x8, face lines): 94.6% to 95.2%, poles 362 to 315, no hole filled;
  Suzanne layouts 96.07% to 96.20% (face x2 92.7 to 93.4, Randall's lines 94.5 to 95.4).

### 0.20.1: no twisted limbs

- **Twisted limbs.** On the girl (zones for thin parts, a neck line, the head at x8, face lines) 0.20.0 left
  the left arm a helix of stretched quads: Patina's own render showed no wire there. Two causes. Holding a
  line along the drawn line's direction took the nearest drawn segment within half a quad, which in the
  hand's margin could be another line's (the finger rings): now only a segment within 0.2 of a quad and
  within 50 degrees of the laid edge, projected into the surface's plane. And a helix has no poles, so
  the score never saw it (0.19.7 twisted the other arm, by chance): each try now also counts the share of
  its edges more than 15 degrees off its own field (QuadriFlow's Q at the nearest working vertex), at
  half weight. On the girl's body the twisted try had 30% of edges off, the clean ones 9-11%.
- The stronger rule for sharp edges in ComputeIndexMap (offset across always 0) is kept to drawn lines
  (Parametrizer::patina_line_edges); open edges, also sharp, keep upstream's rule (a zigzag margin held
  that way made whole parts collapse).
- A group's cache key counts only the lines within reach of its own vertices (a box took in the face for
  the body, through the hair).
- Numbers: Suzanne layouts 95.82% to 96.07%, poles 1,815 to 1,703 (0.19.7: 95.22%, 2,066); the girl with
  ten body loops 94.3% to 95.1%, both arms clean (lean of the arm's edges off its axis 4-7 degrees, a
  twisted arm 17); multi-resolution set 96.0% to 95.5% (falls back more often: noisy). The thin-parts GUI
  test's ball now gets its finger as a four-sided tube at 400 per m², so the test asks at 300.

## Further reading (2026-10-16)

The papers that address what is left, and what each would mean here:

- Pietroni, Nuvoli, Alderighi, Cignoni, Tarini, "Reliable Feature-Line Driven Quad-Remeshing", SIGGRAPH
  2021 (QuadWild): feature lines reproduced by construction; field, then a patch layout bounded by the
  lines, counts assigned for all patches at once, each patch filled from patterns. The closest match to
  Patina's aim. Code GPL-3.0 (github.com/nicopietroni/quadwild; needs Gurobi), a fork with the Bi-MDF
  solver instead (github.com/cgg-bern/quadwild-bimdf, GPL-3.0): not usable in Patina as code.
- Takayama, Panozzo, Sorkine-Hornung, Sorkine-Hornung, "Sketch-Based Generation and Editing of Quad
  Meshes", SIGGRAPH 2013, and "Pattern-Based Quadrangulation for N-Sided Patches", SGP 2014: sketched
  patch borders, edge counts per border, patterns placing the poles (CozyBlanket's way). Patented
  (US 9,349,216, Disney): to be checked before building something close to it.
- Heistermann, Warnett, Bommes, "Min-Deviation-Flow in Bi-directed Graphs for T-Mesh Quantization",
  SIGGRAPH 2023: the counts of a patch layout solved exactly and fast; libSatsuma
  (github.com/cgg-bern/libSatsuma) is MIT and builds on LEMON, already here. The natural next step for
  the ring counts and the steps where zones join (planRingCounts plans each chain of rings alone).
- Lyon, Campen, Kobbelt, "Quad Layouts via Constrained T-Mesh Quantization", Eurographics 2021.
- Bommes, Zimmer, Kobbelt, "Mixed-Integer Quadrangulation", 2009; Bommes et al., "Integer-Grid Maps for
  Reliable Quad Meshing", 2013: lines as integer constraints of one global parametrization.
- Campen, Bommes, Kobbelt, "Dual Loops Meshing", 2012: layouts from loops (round eyes, mouths).
- Knöppel, Crane, Pinkall, Schröder, "Globally Optimal Direction Fields", 2013: curvature alignment in
  the field (the limb helix; it cost poles when tried here).
- Jakob, Tarini, Panozzo, Sorkine-Hornung, "Instant Field-Aligned Meshes", SIGGRAPH Asia 2015:
  brush-driven, interactive; QuadriFlow builds on it.

The large step these point to: lines, then a patch layout, then one count solve, then each patch filled
from a pattern, instead of QuadriFlow per zone and joining rows. Most poles left are where zones join
or rings step; patches would also make edits local (only the patches a line touches filled again).

## 0.20: lines followed by QuadriFlow's own edges

Looking at QuadriFlow's raw quads (before lines are laid into them) showed most poles on Suzanne coming
from lines it didn't follow: its field ran along a line, but its edge loop sat up to half a quad off it
and jumped a row now and then, so laying the line afterward zigzagged and joins needed 3-to-1 steps (the
face's edge loop round the nose had 30 points against the ring's 14).

- **Held along the drawn line.** Each laid line's working vertices were held (position and direction
  constraint) along their laid edge, a path along the working triangles up to 30 degrees off the line;
  a lattice line held along that zigzag misses the line. They are now held along the drawn line's own
  direction there (the nearest drawn segment, in QuadriFlow's normalized space).
- **No row jumps across a line** (third_party/quadriflow parametrizer.cpp, ComputeIndexMap). A sharp
  edge's integer offset across it is always set to 0, and which way is across comes from the edge itself;
  upstream only did this when both offsets were non-zero, from the position field's difference, and
  compared qy.d without abs.
- **Tries judged by the lines.** Each try's score adds the length (in quads) of the laid lines no edge of
  it runs along (within 0.2 L, 25 degrees), counted like poles.
- **Rings take the loop beside them more often** (matchFixed in RetopoRings). A zone's edge loop is taken
  as a ring's loop when all but a few of its points (max(3, n/8); n/5 since 0.20.7) are within 0.4 L of the line; those
  few (within 1 L) are put onto the line, and points that step back or crowd where the loop jogs are slid
  along it halfway between their neighbors. The zone's vertices are moved there too, unless one of its
  faces along the loop would turn over or is a sliver (under 0.03 of a quad): then the ring is made again
  without that loop and joined by a row (a narrow band on a ball had a row of slivers along the line).

Numbers (15 Suzanne layouts at 1,000-1,200 per m2: the bench, Randall's lines, face x2, 12 jittered):
95.2% to 95.8% four-edged, poles 2,066 to 1,815; Randall's lines 93.1% to 95.4%. With the face at x2 on
the 12 jittered layouts, the girl's head at x3: 95.6% to 96.0%. A circle and a wavy line on a ball (no
rings): 98.4% to 99.2%, poles on the lines 17 to 0 (test curved_lines_are_followed_without_poles_along_them).
The girl with ten body loops (neck, waist, wrists, arms, thighs, ankles): 94.0% to 94.3%, 121 to 107 poles.

Tried and left: a margin edge (where a group's sub-mesh is cut) no longer held or sharp (mixed: 96.0% to
95.7% on the multi-resolution set); a narrower margin (no change in the lines left).

### 0.19.7: zone groups made once

- **Group cache** (FlowCache::groupsMade): each zone group's QuadriFlow quads are kept, the last eight,
  under a hash (FNV-1a) of its sub-mesh (positions, triangles), density, sharp edges, quads per m2, the
  knobs, and the lines and strokes whose points fall within its box plus two of its quads. A build where
  only another group's lines changed takes them as they were; a build with nothing changed is instant
  (Suzanne 2.5 -> 0.33 s; the girl 2.8 -> 0.4 s). The wall time of a change is still the changed group's.
- The join mending leaves a loop lying mostly along the model's own open edge open.
- Open edges are freed after two passes far off with them held (the second already corrected), or at once
  when the best try misses over 5% of the surface; one far-off pass is QuadriFlow's usual shortfall on a
  refined model.
- GUI stress with the Flow, Density and Zone tools as well (120 steps), and a sixth fuzz round (162 cases):
  no crash.

### 0.19.6: zones keep their resolution; open models

- **Zone resolutions carried over** (`Engine::retopoSetLines`): when the lines change, every zone with no
  resolution set inside it takes the one most of its faces had before, as a seed recorded in the same undo
  step. The eyes drawn on a head at x8 were left at x1 (2 cm eyes with 1.6 cm quads). Test
  tests/api/test_retopo_zones.py.
- **Open edges set free when holding them goes wrong**: QuadriFlow's tries hold the model's open edges (and
  a group's margin edges) along themselves; on a hood that sent the count to 24% and 468% of the count
  asked for. A pass whose mean count is under 35% or over 300%, or whose best try misses over 5% of the
  surface (far from the quads, or quads off it: a torn hem), is followed by one with them free and the
  count correction reset (only where the group's mesh has open edges). Measured: the hood 6,113 quads,
  100%, its edge within 1.3 quads, 8.6 s (was 1,459 coarse quads, 48 s); a fuzz skirt case: hem within
  1.5 quads, nothing left out, 8.6 s (was torn, 9.2% left out, 31 s). Suzanne, the girl, the jittered
  layouts unchanged.
- Tried and dropped: half the tries with open edges free (a group's margin edges are open too, so every
  build changed and Suzanne lost its rings); extra smoothing round sliver faces and taking out vertices
  with two edges (Suzanne's slivers are corners of its open eye sockets, not joins).
- Fuzzing open shapes (skirt, sheet, hood; 350 cases): no crash; median 97-99.5% four-edged.

### 0.19.5: a crash in QuadriFlow's solver

- A GUI stress script (random Lines drags, Ctrl+Z, redo, rings on and off, density and zone resolution
  changes, Make quads, symmetry, while the live preview rebuilds and is canceled; Suzanne and the girl, 360
  steps) and a third fuzz round (195 cases) each found a segfault in SparseLU's solveInPlace: QuadriFlow
  solved with a failed factorization. The solver's info() is now checked (position solves keep what they
  had, the scale solve drops the try). After the fix the same runs pass with no crash.
- A pass where no QuadriFlow try gives any quads (a stretched ball at 600 quads: every lattice collapsed)
  is followed by one asking for three times as many (up to x8).

### 0.19.4: fixes from fuzzing

A fuzz script (random shapes: ball, egg, finger, bumpy ball, torus, Suzanne, the girl; 0-6 lines from plane
cuts, some open; random zone resolutions, rings on or off, mirror; 200-3,000 quads) ran 340 cases with no crash.
It found:
- **A group of only empty regions** (left where lines cross) when every other zone was a ring or a disc:
  that group failed and so did the build. It now makes nothing.
- **A ring over a bulge**: two lines round the back of a head; the rows are as many as the narrow side
  holds, so the bulge got rows far apart (1,138 quads where 4,221 were asked, 40% flagged as left out).
  `fillRings` now refuses a ring with under half the quads its surface asks for, quads covering under
  75% or over 140% of its surface, or rings over 1.6 times as long as its loops; the zone is made as the
  others are.
- Heavy builds: QuadriFlow's count is kept within 15% (was 12%): a 12,000-quad egg 48 -> 32 s.
- **Joins mended in place**: faces the welds left with a vertex twice become triangles or go; up to six
  edges with more than two faces have the joining faces round them taken out and the holes closed again
  (gridOrSplit, else ring by ring). One bad edge used to send the build through every fallback (Suzanne,
  face x2 / snout x3: 6.5 -> 2.1 s).
- **Coarse models refined** even with nothing drawn (under 8 working triangles per quad): QuadriFlow failed
  on a stretched ball's long thin triangles. Its count correction goes up to x8 (that ball first gave 29%).
- **Symmetry**: only an open stroke's ends (or all of a stroke along the middle) are put on the middle, and
  a mirror image within the deadzone of the stroke is not added: a 2 cm loop round a fingertip with a 1.8
  cm deadzone folded flat and got a copy on top of it.
- A second fuzz round (150 cases and 12 on the girl) found the last three.
- Tried and dropped: a coarser working surface away from the lines (0.6 or 0.5 of the quads' size
  there): quicker and as good on Suzanne, but QuadriFlow's counts went erratic on coarse models and
  more joins fell back.

### 0.19.3: fixes

- A disc is closed with a grid only while 4 pi A / P^2 < 2.5 (a dome; an eyeball is 1.55): with a
  shoulder line the arm is a ring, the hand past the wrist a disc beside it only, and a grid over it (its
  surface many times what its wrist loop holds) lost the fingers. Test
  `a_long_part_past_a_ring_is_not_closed_with_a_grid`.
- QuadriFlow: exit() on inconsistent states and NaN equations throws instead (the try is dropped); its
  sparse solves skip an empty system (SparseLU divided by zero, SIGFPE).

### 0.19.2: rings only where they pay

- **Rings demoted by their cost** (`planChains`, `ChainPlan::cost` in RetopoQuads): when the zones are
  found (before QuadriFlow), the chains of ring zones are planned (as in 0.19.0) and for each ring that
  meets an ordinary zone the plan is made again without it; the ring whose going lowers the steps in all
  the most is made with the zone beside it, one at a time, while that saves more than four steps. The eye
  socket band (32 round inside, 57 outside) goes; QuadriFlow turns the flow there with a few poles and
  the line is laid into its quads. Large discs are classed before (so a disc beside a ring counts as such)
  and again after (a disc beside a demoted ring goes with that zone).
- **Plans with the loops rings will take**: after the ordinary zones are made, a ring loop along which
  one of them has an edge loop the ring will take (`matchingLoopCount`) has that count fixed in the plan.
  Loops that take any count prefer even ones (a disc beside them is a grid only with an even count).
- **A zone's loop is taken only when it runs forwards**: every point further along the line than the one
  before (by over 0.15 of a quad), once round in all; QuadriFlow's loop along a line can double back,
  which left quads turned over along it.
- **Odd discs**: a triangle where the loop turns most, the grid on the rest.

Numbers: Suzanne (eye, socket, nose, mouth lines) 95.4%, 2.5 s; the twelve jittered layouts 94.6-97.1%,
eleven with no fallback; Randall's own Suzanne lines 96.5%; face x2, snout x3 96.0%; the girl 94.5%.

### 0.19.1: fewer poles

- **Pole pairs where zones join** (`cleanJoinPoles` in RetopoQuads): after the join, in up to six rounds,
  a quad's two opposite poles are made one where that leaves fewer poles (both free to move, sharing only
  the quad's other two corners, no quad round them turned over), and an edge between two quads is turned a
  step where the poles at its ends and across get fewer. Never moved or turned: vertices of ring zones,
  vertices within 0.2 of a quad of a line, vertices beside triangles or on open edges. A pole on a line
  counts twice, and an edge is added to a vertex on a line only where that gives it four. The vertices are
  not renumbered (unused ones go with the rest afterward).
- **Inset grids**: a disc whose loop is a line (not the model's open edge) gets a plain ring between the
  loop and its grid (the grid's square inset by 0.8 / (n + 1.6) of its side in the map): the four corner
  poles sit inside and every vertex on the line has four edges.
- Tried and dropped: asking QuadriFlow for 8% more quads to save its second run (the girl 2.8 -> 1.7 s,
  but fewer tries to pick from: 94.5% -> 93.1%); remembering the count correction between builds (the same
  build gave different quads each time); 9 or 12 tries instead of 6 (slower, same picks).

Numbers: Suzanne with the eye, socket, nose and mouth lines: 93.2% four-edged (31 poles on the lines, were
43); face x2, snout x3: 96.0%; the girl with zones for thin parts: 94.5%; the jittered layouts 93-97%.

## 0.18: zones for thin parts

- **core/RetopoThin.** Thickness per face: a ray in along -normal from just inside (up to 50 quads), then
  the median of the face and its neighbors, twice. Thin: thickness < 4/pi of the quad size there (fewer
  than four quads round). Parts: thin faces joined across edges. Tube or sheet: from the middle of a thin
  face's thickness, the shortest of six chords in its plane; under twice the thickness is tube-like; a part
  is a tube when 60% of its area is. Resolution: from the part's thinner quarter (by area: fingers, not the
  palm), for six quads round, rounded up to a power of two (2..64).
- **Rings.** Each edge of a part with a large thick region (not a small thick island inside it) becomes
  a plane cut: normal from the edge's middle toward the part's faces near it, plane moved to just beyond
  the part's farthest reach (the edge wanders over a palm whose thickness is near the bar), then back in
  steps of half a quad (0 to 3) until the plane meets only one closed loop of about the edge's size. A
  part with an edge that can't be cut so (toes) is left.
- **Engine.** Lines added, zones found; a part's zone is the one holding most of 64 sample faces, and
  must be under 2.5 times the part's area (else its lines go again); resolutions set; one undo step. Default
  max x16: hair strands needing x32-64 are reported, not zoned.
- **Join.** A closed border with no edge on one side at all (an island zone with no quads) goes to the
  hole filler. Normal rays for the surface projection stop at 0.75 of the smallest quad.
- **Rings keep their count** while the far loop is at most 1.6 times as long (hand-made eye loops).

Numbers: the girl at 4,000 per m²: plain 2,032 quads, 5% left out (hands, hair); with zones for thin parts,
3,617 quads, 93.2% four-edged, nothing left out, 3.5-4.6 s; both hands x16 with wrist rings.

## 0.17: loops view, speed, the preview is the result

- **Loops view (core/RetopoLoops).** Edge loops traced through four-edged vertices whose four faces are
  all quads (the edge across: the one sharing no face with the incoming edge); poles by edge count.
  Drawn: poles as small crosses (blue 3, orange 5, red more) and the loop under the pointer (LoopFinder:
  a grid of edge middles, nearest within 0.75 of a quad) in green, 4 px, pulled toward the eye.
  Coloring every loop was tried and was unreadable.
- **Speed.** QuadriFlow's QuadEnergy (exhaustive hole filling) is memoized per sub-polygon (same
  output); the Suzanne layout went from 5.0 to 2.4 s, and parallel fields no longer serialize on the
  allocator. Coarser refinement away from lines (0.8 of a quad) was a further 25% but cost quality in two
  of three scenes: not kept.
- **Preview.** Made with the full settings (it cost about the same: parallel fields contend), so Make
  quads keeps an up-to-date preview at once.
- **Poles off the lines.** When the side that keeps its line has fewer points, the join makes a plain
  row next to it and the steps after (poles on the drawn lines: 40% to 26%). Moving ring zones' internal
  steps off the end loops crowded one row and was reverted. Rings need room (half a quad wide on
  average), small discs two quads or more and roundish (4πA/P² > 0.25 on the zigzag outline), and a
  ring takes a neighbor's loop when both lie within 0.4/0.6 of a quad of each other.

## 0.16: zones joined anywhere, rings between two lines

Randall gave a hand-made Suzanne as the target (concentric loops round the eyes, nose and mouth, rims
round the ears, poles hidden in the eyeballs and the mouth) and CozyBlanket's videos: guides become loops
exactly, the fill between is structured and predicted live, edits stay local.

- **Joins (core/RetopoJoin).** The borders between groups are split into chains at points where three
  groups (or an open edge) meet. Along each chain the coarser side's faces touching its edge there are
  taken out; each group's boundary loops are labeled by their nearest chain and split into runs; runs
  facing each other (opposite loop directions, overlapping along the chain) are paired, a run shared
  out between its pairs vertex by vertex. Each pair gets a stepped row: the steps (1, 3, the odd 2, or 5)
  come from a search for the shortest rungs; more than about 2.2 edges per edge is split into two or
  three rows (x16 hands). The holes left where chains meet are filled ring by ring (a row like the joins'
  to a ring an edge in, then cut up), needles zipped, tiny edges welded; bigger patches are marked in red.
  A closed (or long) chain with no row at all fails: its two sides would only be capped.
- **Before the join.** Regions smaller than about a quad (slivers between near lines) go to the
  neighbor they share most line with. Parts of a group too thin for it (an opening: narrower than 1.5
  of its quads, 2.5 when it is coarser than a neighbor) and necks (closer than that to two different
  chains) go to the group beside them. A per-group "small hole" fill no longer caps a thin limb's end
  that lies along a border.
- **Rings (core/RetopoRings).** A region with two boundary loops and no handles is a ring zone, if it
  is a band (area at most 1.6 x width x mean loop length). u: harmonic (cotangent weights, conjugate
  gradient), 0 on the shorter loop, 1 on the other; rings are its level lines at even mean distances
  (rows: as many as the narrow side holds); counts step evenly from one loop's (measured on a level line
  just inside it: the loops zigzag) to the other's, kept the same when within 15%. A disc of under ~24
  quads is a cap: rings shrinking in, then cut up. Rings and caps are made one after another; a loop a
  ring beside made already is taken as it is (shared, welded, no join); their vertices are not smoothed.
- **Projection.** A vertex goes to the nearest surface only if it faces the vertex's way; otherwise along
  its normal (eyeballs in their sockets).
- **Preview.** Quad edges are pulled toward the eye by 0.3% of the view distance instead of a depth
  offset (which, with the tight near plane, showed the far side through the model).

Numbers: the Suzanne layout above at 1,200 per m²: 3,405 quads, 88.5% regular, 4.3 s; rings round both
eyes, the nose and the mouth, the tip of the nose and the lips closed. The 0.15 face/snout scene: 3,710
quads, 94.0%; the girl's hands at x16 join at both wrists (14 3-to-1 steps).

## 0.15: the model's own surface, live preview, symmetry

- **Working surface.** Big models (over 200k triangles, or more than 40 per quad) are decimated
  (core/RetopoDecimate: quadric edge collapse with the link condition, no flips, open edges held, no
  edge over twice the target), in parallel over balanced pieces with their borders frozen, then once
  over the whole (1.9M triangles in 0.6 s). The target is a third of the local quad size (zone
  resolution and density per vertex, through CleanMesh::origin). Coarse models are refined the other
  way (red-green splits) only where the local quad size needs it. The voxel shell is only used when
  asked for: it joined fingers and wrapped the legs and hair. After a build, points of the working
  surface further than half the local quad size from the quads are marked (Result::uncovered, red
  crosses) and the share said: parts thinner than the quads (fingers at the default resolution) are
  left out by QuadriFlow. The working surface is cached between builds (FlowCache, keyed on the mesh,
  quads per m², density and zone resolutions).
- **Live preview.** Engine::retopoTick starts a quick build (3 fields, 2 count passes) 0.35 s after the
  inputs last changed while the UI asks for it (RetopoState::wantPreviewAt); retopoChanged cancels a
  running preview; the cancel flag is also checked inside QuadriFlow's long loops (patina_check_cancel),
  and its open-ended loops are guarded (patina_loop_guard) after one went round for ever in FixHoles. A
  watchdog stops a field taking five times as long as the first to finish. Make quads (full build)
  takes over from a preview. Zone groups run in parallel.
- **Symmetry.** core/RetopoMirror: across planes through the bounding box's middle; points within the
  deadzone go onto the plane; runs on the kept (longer) side are mirrored and joined at the plane into
  one line or one loop. Used for lines, flow strokes, their removal, zone resolutions (the mirror zone of
  the clicked point) and the density brush. Quads are still made over the whole model.

Numbers (0.15, the girl at 4,000 per m²): plain 2,032 quads, 96.2% regular, 3.0 s (9% of the surface
too thin: hands, hair); wrist lines and hands at x16: 3,227 quads, 93.7%, 4.6 s, every finger separate;
the next build after a new line reuses the working surface (ready at 0.01 s instead of 1.85 s).

## 0.13: painted patches (method "patches")

0.13's patch filling is still there (`retopo_build` with `method: "patches"`), described below.

## What it is

You decide where the patches of quads go; Patina fills each one with clean quads at the density you
set. The high-poly model stays as it is and becomes what the low-poly is baked from.

- **Patches, painted.** Paint a region of the high-poly the way the Island brush paints an island:
  it becomes a patch. Where patches meet are the low-poly's main edge loops; where three meet is a
  corner. A patch can be a disk (a cheek, the back) or a ring (an arm, a finger, the neck).
- **Density, per m², painted.** A target in quads per square meter for the whole model (shown as
  the edge length it means), and a density brush that raises it where you paint (face, hands) and
  lowers it elsewhere.
- **Quads that fit.** Edge counts are worked out for every patch boundary together, so neighboring
  patches always share their vertices: opposite sides of a four-sided patch get the same count, a
  ring gets the same count around both ends, and three-, five- and six-sided patches get one pole in
  the middle. The quads lie on the high-poly's surface and are relaxed along it.
- **Then into Patina.** The result can be exported, sent to Blender as a new object, or made the
  model you paint, with the high-poly as the bake source (normals, AO, curvature from it).

## How (milestone 1)

1. Patches are face labels on the high-poly. Before building they are tidied: specks smaller than
   about two quads join the patch around them (small gaps fill), and where a patch touches itself at
   a single vertex the smaller fan of faces there goes to the neighbor.
2. Patch boundaries are cut into curves where the neighboring patch changes (a junction). Each disk
   patch chooses its corners from its junctions and its sharpest bends, all at once, by a score:
   sharp corners, no tiny sides, opposite sides of a four-sided patch about the same length, a pole
   that fits for three, five or six sides, four sides preferred. A patch runs straight through the
   junctions it doesn't take (best where it is nearly straight and no other patch there does too:
   one patch of a Y, the stem of a T), so most junctions end up with four edges. A patch with fewer
   than three junctions (a cap) gets four corners at its sharpest points. A ring patch (two boundary
   loops) has no corners.
3. Counts: every curve gets a target from its length and the density along it, and an even count, so
   every patch can always be filled. Opposite sides of four-sided patches and the two ends of rings
   are asked to be equal (least squares with equalities, rounded a few at a time) unless the painted
   density asks for a change there or the shape is more than 1.6 to 1; then edge loops end in steps.
4. Each patch is flattened onto a convex polygon whose sides take room in proportion to their counts
   (a ring onto an annulus): a Tutte embedding solved on a ladder of coarser graphs, then reweighted
   three times so its areas follow the surface's (a plain one shrinks the middle of a bulge).
5. The quads are laid out in the plane: a four-sided patch is rows from one side to the opposite one;
   three, five and six sides get a pole in the middle and a four-sided part at each corner, the pole's
   spokes chosen so the parts are as near plain grids as can be; a ring is rows from one end to the
   other. Each row line is as wide as the patch is there (its length on the surface times the
   density), so loops are added where the patch widens or the density rises and end where it
   narrows, in three-to-one steps (two three-valent and two five-valent vertices). Where both pairs of
   sides differ, a chain splits the patch into a part whose columns change and one whose rows do.
6. The points are mapped back onto the high-poly, boundary samples are shared (welded), and the inside
   vertices are relaxed along the surface.

Later: guide strokes that split patches and steer the flow, corners you place by hand, PolyBuild-style
editing of single quads, symmetry.

## Status

| Step | State |
| --- | --- |
| Core: tidy, patches, corners, counts, layouts, flattening, welding, relax | done (tests/test_retopo.cpp) |
| Commands for scripts and tests (`retopo_*`) | done |
| Patina: Patch and Density brushes, Retopology menu and window, overlays (patches, density, the quads), background build, kept between sessions | done (tests/gui/test_retopo_brushes.py) |
| Export .obj, send to Blender (add-on 0.7.0, tests/blender/test_live_bridge.py), use as the model with the high-poly baked | done |
| 0.14: QuadriFlow, zone lines, zones with resolutions joined by three-to-one rows, flow strokes, working shell (tests/test_retopo.cpp, tests/gui/test_retopo_brushes.py) | done |
| Junctions of three resolutions, narrow zones, corners by hand, PolyBuild editing, symmetric result | later |

Numbers on the sample (1.93 million triangles, 0.54 m², 4,000 quads per m² = about 2,140 quads): 60
suggested patches (split to 154 where cells weren't disks) give 2,480 quads in 2.0 s; a hand-style layout of 24
patches builds in 3.4 s. Painting: a stroke across both thighs (85,000 faces) in one go.

What is still rough: suggested patches are cells of about the same size, not a layout an artist would draw (they
give more poles than a painted layout); hair and other tangled parts of generated meshes need patches painted with
care (or are left out with the reason); counts run 10 to 20% over area × density where patches are lopsided.
