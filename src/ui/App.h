#pragma once
#include <imgui.h>
struct ImRect;

#include <atomic>
#include <future>
#include <thread>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ui/FrameStats.h"
#include "core/UVOps.h"
#include "engine/Engine.h"
#include "engine/SmartMaterial.h"
#include "AppArgs.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Tr.h"
#include "ui/PenInput.h"
#include "platform/HttpApi.h"
#include "platform/Net.h"

struct SDL_Window;

namespace pfd {
class open_file;
class save_file;
class select_folder;
}  // namespace pfd

namespace patina {

struct Args;

const char* viewModeLabel(int mode);  // (Viewport3D.cpp)
int fuzzyScore(const std::string& text, const std::string& query);  // the command palette's match (Palette.cpp): < 0 none
bool menuHeader(Engine& engine, const char* key, const char* label);  // a menu bar menu (Panels.cpp)

void openFolder(const std::filesystem::path& p);  // (in the system's file manager; made first if it isn't there)

enum class ViewportTool { Paint = 0, Erase, Blur, Fill, Picker, SelectFaces, Seams, Clone, Island, Smudge, Note, Patch, Density, ZoneLine, Flow,
                          Object /* (1.2) */ };
enum class UVMode { Edit = 0, Paint };
enum class UVSelectMode { Face = 0, Island, Vertex };

// A rebindable shortcut (Edit > Keyboard shortcuts, saved in keymap.json)
struct KeyAction {
  const char* id;
  const char* label;
  const char* group;
  ImGuiKeyChord patinaChord;   // default in the Patina keymap
  ImGuiKeyChord blenderChord;  // default in the Blender keymap
  ImGuiKeyChord defaultChord;  // default of the active keymap
  ImGuiKeyChord chord;         // current binding
};
enum class KeymapPreset { Patina = 0, Blender, Count };
const char* keymapPresetName(KeymapPreset p);
std::string chordName(ImGuiKeyChord c, bool pretty = false);  // pretty: for display ("Numpad /" not "KeypadDivide")
ImGuiKeyChord chordFromName(const std::string& s);

enum class NavPreset { Patina = 0, Blender, Maya, ZBrush, Count };
const char* navPresetName(NavPreset p);

// Interactive UV transform (G/R/S style)
struct UVTransformState {
  bool active = false;
  char kind = 'g';                 // g move, r rotate, s scale
  vec2 startMouseUV{0};
  vec2 pivot{0};
  std::vector<uint32_t> loops;
  std::vector<vec2> original;      // full UV array before the transform
  std::vector<vec2> preview;
  bool dragMove = false;           // started by dragging the selection
};

class App {
 public:
  int run(const Args& args);

  // --- shared state used by the panels
  Engine engine;
  SDL_Window* window = nullptr;
  ViewportTool tool = ViewportTool::Paint;
  FrameStats frameStats;  // (PATINA_FRAME_STATS=<file>: frame timings, for profiling)
  uint64_t lastViewKey = 0;    // (0.22.2) what the 3D view last drew (reused while a line is drawn)
  int uvLastW = 0, uvLastH = 0;
  // Seams tool: drag along the model and the seam follows the edges between the vertices under the
  // cursor (Ctrl clears); Shift+click continues from where the last seam ended
  uv::SeamGraph seamGraph;
  struct SeamStroke {
    bool active = false, clear = false, moved = false;
    uint32_t last = uv::SeamGraph::kNoVertex, startFace = 0;
    vec3 startPos{0.f};
    std::vector<uint32_t> wedges;
  } seamStroke;
  uint32_t seamLastEnd = uv::SeamGraph::kNoVertex;
  std::vector<uint32_t> seamPreview;  // Shift held: the path the click would add
  uint32_t seamPreviewTo = uv::SeamGraph::kNoVertex;
  PaintTool lastPainterTool = PaintTool::Paint;  // detects tool changes made by commands (MCP / console)
  uint64_t lastToolRequest = 0;                  // (0.36) set_tool requests seen
  bool cloneMarkerValid = false;  // 3D position of the clone source (for the viewport marker)
  vec3 cloneMarkerPos{0.f};
  vec2 cloneMarkerUV{-1.f};  // the source UV the marker belongs to
  UVMode uvMode = UVMode::Edit;
  UVSelectMode uvSelectMode = UVSelectMode::Island;
  int uvTextureSource = 0;  // see UVEditor.cpp
  bool reprojectOnUVEdit = true;
  vec2 uvViewCenter{0.5f};
  float uvViewZoom = 0.9f;
  UVTransformState uvXform;
  std::vector<uint8_t> uvLoopSel;  // UV-vertex selection (per loop), used in Vertex select mode
  std::vector<uint32_t> uvSelectedLoops();
  RenderTarget viewportRT, uvRT;
  vec2 lastViewportSize{1280, 800};
  ImVec2 lastViewportOrigin{0, 0};  // window coordinates of the 3D view (pen injection)
  float uiScale = 1.f;
  PenInput pen;                  // tablet + mouse samples at the device rate
  bool showPenTest = false;
  void drawPenTest();
  void drawPenPrefs();
  void registerPenCommands();
  // notes pinned to the model (NotesUI.cpp)
  void drawNotes();
  bool notesOverlay(ImVec2 origin, ImVec2 size, bool hovered);  // true: the Note tool / a pin has the mouse
  void beginNoteEdit(uint32_t id, ImVec2 at);
  uint32_t addNoteAt(vec2 viewportPx, const std::string& text);
  bool showNotePins = true, showResolvedNotes = true;
  uint32_t noteEditId = 0;
  std::string noteEditBuf;
  ImVec2 noteEditPos{0, 0};
  bool noteEditOpenRequest = false, noteEditFocus = false, noteEditIsNew = false;
  // split / compare view (CompareUI.cpp)
  void compareMenu();
  void toggleCompare();
  bool compareOverlay(ImVec2 origin, ImVec2 size, int pw, int ph, bool hovered);  // true: divider has the mouse
  RenderTarget compareRT;
  // (0.37) the X/Y/Z axes in the 3D view's corner (ViewGizmo.cpp): a click on one looks along it, a drag orbits
  bool axisGizmoInput(ImVec2 origin, ImVec2 size, bool hovered);  // true: the gizmo has the mouse
  void axisGizmoDraw(ImVec2 origin, ImVec2 size);
  struct AxisGizmo {
    bool pressed = false, dragged = false, over = false;
    int hot = -1;            // the axis end under the pointer: 0..5 = +X -X +Y -Y +Z -Z
    double hotSince = 0;     // (its tooltip after a moment)
    ImVec2 pressAt{0, 0};
    // the turn to a view: from, to, when it began (the camera's yaw and pitch); only with animateViews (0.38)
    bool turning = false;
    float fromYaw = 0, fromPitch = 0, toYaw = 0, toPitch = 0, setYaw = 0, setPitch = 0;
    double turnStart = 0;
    Camera before;           // (the view before the turn: the one to go back to)
    bool backHot = false, wasBackHot = false;  // (0.38) the pointer is on the Back button under it
  } gizmo;
  bool gizmoBackShown() const;  // the Back button: on a side view with a view to go back to
  // (0.38) side, top and bottom views (ViewNav.cpp): the numpad, the gizmo and Alt while orbiting jump to them
  // (orthographic with autoOrtho), and Numpad 0 goes back to the view before (Engine::viewMemory)
  bool autoOrtho = true;       // side views are orthographic until you turn away (Blender's Auto Perspective)
  bool animateViews = false;   // turn to them in a quarter second instead of jumping
  bool viewSnapping() const;   // the view_snapping feature
  void snapToView(float yaw, float pitch, const Camera* before = nullptr);
  void goBackView();
  // an orbit drag, kept until its button is let go; Alt (Shift for an Alt+left or right-button orbit) snaps it
  struct OrbitDrag {
    bool on = false;
    int button = 0;
    float yaw = 0, pitch = 0;  // where the drag alone would have turned the camera
    Camera start;
    bool startSnapped = false, snapping = false;
  } orbitDrag;
  // returns true while the drag is an orbit (the caller turns the camera itself otherwise)
  bool orbitDragInput(bool orbitNow, ImVec2 md);
  // (0.38) the view pie (ViewPie.cpp): hold Z over the 3D view, point at an item, let go. (0.40) Four pies: Z the
  // view, Shift+Z the channels, ` the edit modes (the tools), Q Quick Favorites (eight of the user's own)
  enum class PieKind { View = 0, Channels, Edit, Favorites, Count };
  struct PieItem {
    std::string id;
    const char* icon = "star";
    std::string label, desc;
    const char* group = "";  // (English, for the favorites picker: T_() it to show)
    bool on = false, enabled = true;
    std::function<void()> run;
  };
  bool viewPieInput(ImVec2 origin, ImVec2 size, bool hovered);  // true: the pie has the mouse
  void viewPieDraw();
  std::vector<PieItem> pieCatalog();            // everything a pie can hold (the favorites can be any of them)
  std::vector<PieItem> pieItems(PieKind kind);  // a pie's eight, in slot order
  bool runPieItem(const std::string& id);       // as if chosen in a pie (false: unknown or not available now)
  void toggleViewMode(ViewMode m);              // (chosen again: back to the mode before)
  static std::vector<std::string> defaultFavorites();
  std::vector<std::string> favorites = defaultFavorites();  // Quick Favorites: 8 item ids ("" = an empty place)
  bool addFavorite(const std::string& id);      // into the first empty place (false: full)
  void favoriteMenuItems(const std::string& id);  // Add to / Remove from Quick Favorites (in a right-click menu)
  void favoritePickerUI();                      // inside a popup: search and choose the item for pie.pickSlot
  void drawPiePrefs();                          // Preferences > Pie menus
  void registerPieCommands();                   // MCP pie_menus
  // (0.39) the path tracer's settings (the top bar's popup, the Render image dialog)
  void pathTraceSettingsUI();
  bool renderPathTraced = false;
  int renderSamples = 256;
  struct ViewPie {
    bool open = false, sticky = false;  // sticky: the key was tapped, so a click chooses
    PieKind kind = PieKind::View;
    const char* key = "view.pie";       // the shortcut that opened it (held: letting go chooses)
    ImVec2 center{0, 0};
    double openedAt = 0;
    int hot = -1;
    ViewMode modeBefore = ViewMode::Material;  // (Solid, Unlit and the channels go back to it)
    // right-click on an item: its menu; an empty Quick Favorites place: the picker (both popups of the 3D view)
    bool openMenu = false, openPicker = false;
    PieKind menuKind = PieKind::View;
    int menuSlot = -1, pickSlot = -1;
    std::string menuId;
    char pickQuery[96] = "";
    bool pickFocus = false;
  } pie;
  // stencil and text tool (StencilUI.cpp)
  void drawStencilSection();
  // draws the stencil over the 3D view; true while the stencil key is held (the view's
  // mouse input then moves / rotates / scales the stencil instead)
  bool stencilOverlay(ImVec2 origin, ImVec2 size, bool hovered);
  bool keyHeld(const char* actionId) const;  // the action's key is down (modifiers ignored)
  std::string stencilText = "Patina";
  TextStyle stencilTextStyle;
  bool stencilTextLive = true;
  // layer mask UI (MaskStackUI.cpp)
  bool generatorWidgets(MaskGenerator& g, const char* label);
  int maskEffectMenu(bool baked = false);  // inside a combo / popup: returns the chosen effect type or -1 (baked: no Paint)
  bool maskStackWidgets(Layer& l);
  MaskEffect newMaskEffect(int type);  // with the usual blend and settings for its type
  bool maskPatternTarget(Layer& l);    // the last item: a material dropped on it adds a Pattern effect
  bool patternWidgets(MaskEffect& e);
  void editorPreview(const MaterialDef& m, float size);
  bool viewModeAvailable(ViewMode m) const;  // its feature is switched on
  int openFxNext = -1;  // the mask effect whose settings open next frame (just added)
  // layer list thumbnails (LayerThumbs.cpp): content and mask, refreshed a few per frame
  struct LayerThumb {
    Texture2D content, mask;
    double updated = -1.0;
    bool hasContent = false, hasMask = false;
  };
  std::map<uint32_t, LayerThumb> layerThumbs;
  int thumbBudget = 0;
  const LayerThumb& layerThumb(const Layer& l);
  struct ScribblePoint { vec2 p; float w; bool start; };
  std::vector<ScribblePoint> scribble;
  // stroke helpers. strokeStartSample: index of this frame's first sample with the tip /
  // button down (-1: none). feedStroke: adds the frame's down samples from `from` on to the
  // active stroke (positions mapped by toLocal) and returns how many were added.
  int strokeStartSample() const;
  StrokePoint strokePointOf(const PointerSample& s, const std::function<vec2(vec2)>& toLocal) const;
  int feedStroke(Painter& P, int from, const std::function<vec2(vec2)>& toLocal);
  bool penEraserStroke = false;  // this stroke uses the pen's eraser end
  bool uvStrokeStartedThisFrame = false;
  int strokeOwner = 0;  // 0 none, 1 3D viewport, 2 UV editor
  bool showDemo = false;
  bool showHelp = false;
  bool showExport = false;
  bool showMaterialEditor = false;
  bool showAbout = false;
  bool requestLayoutReset = false;
  int frontTabsIn = 0;  // (0.47) frames until a fresh layout's front tabs are set (Materials, Properties)
  // ---- quality of life (0.3)
  bool showKeymap = false;
  bool distractionFree = false;
  double distractionFreeSince = 0;
  float baseUiScale = 1.f;      // from the display
  float userUiScale = 1.f;      // View > UI scale
  float pixelDensity = 1.f;
  float pendingUiScale = 0.f;   // applied between frames (font atlas rebuild)
  NavPreset navPreset = NavPreset::Patina;
  std::vector<KeyAction> keymap;
  KeymapPreset keymapPreset = KeymapPreset::Patina;
  bool ctrlPaintsSecondary = false;  // Blender: hold Ctrl while painting to use the secondary color
  bool strokeUsesSecondary = false;  // the running stroke swapped the colors
  bool showPreferences = false;
  bool openAddLayerMenu = false;     // Shift+A in the Blender keymap
  int radialEdit = 0;                // Blender-style F / Shift+F: 1 brush size, 2 opacity (0.40: 3 flow, 4 hardness, 5 angle)
  bool lmbBlocked = false;           // a click that confirmed something else: ignored until released
  bool selStrokeActive = false;      // an Island / Select drag: its selection change becomes one undo step
  std::vector<uint8_t> selStrokeBefore;
  ImVec2 radialCenter{0, 0};
  float radialStartValue = 0.f;
  // ---- 0.4
  bool showDensity = false, showRender = false, showHighPoly = false, showWelcome = false;
  bool showNewSet = false;
  int islandOrient = 1;            // 0 none, 1 upright, 2 longest axis
  int islandDensityMode = 0;       // 0 auto, 1 layout, 2 fit, 3 screen, 4 custom
  float islandCustomDensity = 1024.f;  // texels per unit (custom)
  std::string renderPath;
  int renderW = 1920, renderH = 1080, renderSS = 2, renderFrames = 0;
  bool renderTransparent = false;
  std::string highPolyInput;
  float highPolyDistance = 0.f;    // 0 = measured (1.1)
  bool hpNormal = true, hpAO = true, hpIds = true;
  int hpSamples = 64;
  int hpPoints = 4;                // (1.1) points per texel
  // ---- 1.1: the bake-down (Tools04.cpp)
  bool showBakeDown = false;
  std::string bakeDownInput;
  int bakeDownSource = 0;          // 0 a model file, 1 the retopology's quads
  int bakeDownPoints = 9;
  int bakeDownRes = 0;             // per texture set or tile; 0: the model's
  float bakeDownDistance = 0.f;    // 0 = measured
  bool bakeDownHigh = true;
  void drawBakeDownDialog();
  void bakeDownNow();  // in two frames, so the window first says it is working (bakeDownIn)
  void bakeDownRun();
  int bakeDownIn = 0;
  void bakeDownReportUI(bool buttons);
  bool distanceUI(float& dist, float radius);
  bool pointsUI(int& points);
  bool meshChangedPending = false, autoReloadMesh = false;
  double lastMeshCheck = 0;
  bool recoveryPending = false;
  std::string recoveryPath;
  uint64_t overlapVersion = ~0ull;
  size_t overlapIslands = 0;
  std::vector<uint32_t> uvCollapsed;  // faces with no UV area (same cache key as the overlaps)
  // ---- 0.7.1: UV editor robustness (UVEditor.cpp)
  struct UVPress {                 // a click in the UV editor, before it becomes a drag or a click
    bool down = false, right = false;
    ImVec2 pos{0, 0};
    vec2 uv{0};
    int face = -1, loop = -1;
    bool shift = false, ctrl = false;
  };
  UVPress uvPress;
  vec2 uvMenuUV{0};                // where the context menu was opened
  bool islandPinNew = true;        // Island brush: pin the islands it makes
  int islandReach = 1;             // (0.43) Island brush: 0 the circle on screen, 1 along the surface
  bool selectVisible = false;      // (0.45) Select tool: only the faces the camera sees (Visible only)
  bool uvShowIslandMarks = true;   // pin and mirrored-half markers in the UV editor
  struct UVIslandInfo {
    Rect2 box;
    int faces = 0;
    bool pinned = false;
    int copies = 0;                // faces stacked onto this island from the other half
    bool isCopy = false;           // a stacked mirror half
    int tile = 0;                  // texture tile (texture sets / UDIM)
  };
  std::vector<UVIslandInfo> uvIslandInfo;
  uint64_t uvIslandInfoKey = ~0ull;
  int uvUnstacked = 0;             // mirror copy faces that drifted off their other half
  std::string uvToastText;         // a short message shown in the UV editor (pinned islands and the like)
  double uvToastTime = 0;
  void uvToast(const std::string& s);
  void updateUVIslandInfo();
  enum class UVMenuFrom { Bar, Editor, Context };  // (1.2.7) one UV menu in all three places
  void uvOpsMenu(UVMenuFrom from);
  void selectOtherHalf();
  void faceSelectItems(bool uvEditor);  // (1.2.7) the Select menu's common part (UV menu, 3D view)
  void drawUndoPrefs();
  // (0.37.1) also the 3D view's Pin / Unpin buttons and Alt+P there; `uvVertices`: with no faces selected, the faces
  // round the selected UV vertices (the UV editor's vertex mode)
  void uvPinSelection(bool pin, bool uvVertices = true);
  size_t pinnedSelectedFaces() const;  // the selected faces that are pinned
  void uvFrameSelection();
  bool uvRunOnSelection(const char* cmd, json args, const char* status = nullptr);
  bool envDrag = false;
  bool showWelcomeOnStart = true;
  std::string keymapCapture;    // action waiting for a new key in the shortcut editor
  bool keymapScrollTo = false;  // ... scrolled to (Change shortcut on a toolbar button)
  std::string prefsScrollTo;    // (0.36) open_window preferences {section}: scrolled to it
  std::vector<std::string> recentFiles;
  std::vector<std::pair<std::shared_ptr<std::promise<json>>, int>> frameWaits;  // (0.45) wait_frames
  std::string lastSeenFile;
  std::vector<vec3> swatches;
  std::vector<vec3> recentColors;
  vec3 secondaryColor{1.f};
  bool quickPickActive = false;  // I held: temporary color picker
  bool quickPickUsed = false;
  ViewportTool quickPickReturn = ViewportTool::Paint;
  json layerClipboard;           // copied layer settings
  std::string errorTitle, errorText;
  bool errorPending = false;
  std::string smartSaveName;
  bool smartSaveIncludePaint = false;
  int renameLayerUid = 0;        // layer whose name is being edited inline
  std::string lastSessionDump;
  double lastSessionSave = 0;
  int exportWhat = 0;            // 0 all layers, 1 selected layer only, 2 selected layer's mask
  std::string exportTemplate = "$mesh_$map";
  int focusDefaultTabs = 0;
  std::string materialFilter;
  std::string materialCategory;
  bool materialList = false;   // (1.2.3) materials and smart materials as a list with small pictures (else tiles)
  float materialTile = 84.f;   // (1.2.3) the tiles' picture size, at UI scale 1
  std::string dropHint;  // (0.22.5) what the thing dragged would do where it is (said by the drag's tooltip)
  std::string consoleInput;
  std::string statusText;
  double statusTime = 0;
  int exportPresetIdx = 0;
  int blenderLibSize = 1, blenderLibMapping = 0;  // Blender Link: 512/1024/2048; box/uv
  std::string blenderLibStatus;
  int exportSizeIdx = 0;
  int exportFormatIdx = 0;  // 0 PNG, 1 layered PSD
  bool exportAO = true, exportThickness = true, exportBent = true;
  std::string exportDir;
  std::string exportBaseName;
  MaterialDef editMaterial;       // material editor working copy
  bool editMaterialDirty = false;
  MaterialLibrary::Entry* editPreviewEntry = nullptr;
  net::JsonLineServer server;
  net::HttpApi httpApi;           // 0.5: HTTP API for other applications
  bool httpEnabled = false, httpLan = false;
  int httpPortSetting = 7870;
  std::string httpError;
  Args launchArgs;
  bool startHttpApi(bool fromSettings);
  void drawNetworkPrefs();
  int serverPort = 0;
  uint64_t lastCompositeShown = 0;

  // pending native dialogs (polled every frame)
  std::unique_ptr<pfd::open_file> openDialog;
  std::unique_ptr<pfd::save_file> saveDialog;
  std::unique_ptr<pfd::select_folder> folderDialog;
  enum class DialogPurpose { None, OpenFile, SaveProject, ImportMaterial, ExportDir, LoadHDR, BrushImage, HighPoly, RenderPath,
                             StencilImage, StencilSave, FontFile, RetopoExport, BatchFiles, BatchFolder, BatchOutput,
                             ModelExportDir, BakeDownLow, AddObject, ImportBrushes };
  DialogPurpose dialogPurpose = DialogPurpose::None;

  // window capture request from MCP
  std::shared_ptr<std::promise<json>> capturePromise;
  json captureArgs;

  void setStatus(const std::string& s);
  void openFileDialog(DialogPurpose purpose);
  void saveProject(bool saveAs);
  void doUndo();
  void doRedo();
  void showError(const std::string& title, const std::string& text);
  void noteModelTextures();
  std::string deferredStatus;  // set on the next frame (after the caller's own message)
  bool keyPressed(const char* actionId, bool repeat = false) const;
  // ---- command palette (Palette.cpp)
  struct PaletteEntry {
    std::string label, hint;
    std::function<void()> run;
  };
  std::vector<PaletteEntry> paletteEntries;
  std::vector<std::string> paletteRecent;  // labels, most recent first (saved with the session)
  bool showPalette = false, paletteFocus = false;
  char paletteQuery[256] = {};
  int paletteSel = 0;
  std::string pendingAction;         // a shortcut action the palette runs on the next frame
  mutable std::string forcedAction;  // ... and the frame it runs in: keyPressed() answers true for it
  void openCommandPalette();
  void buildPaletteEntries();
  void drawCommandPalette();
  bool showMacros = false;
  // 0.10: mesh symmetry check
  bool showSymmetry = false;
  bool showMeshCheck = false;  // 0.27
  bool showModelExport = false;  // 0.28
  bool showReduce = false;       // 0.29
  float reducePercent = 50.f;
  bool reduceKeepBorders = true;
  json reducePreview;
  float reducePreviewFor = -1.f;
  std::string reduceStatus;
  int modelExportLods = 0;       // levels of detail besides the model (each half the last)
  // 0.30: recipes and batches
  bool showBatch = false;
  std::string batchRecipe = "patina_game_asset";
  std::vector<std::string> batchPaths;
  bool batchRecursive = true;
  std::string batchOutput;
  bool batchEditing = false;
  std::string batchEditName, batchEditDesc, batchEditSteps;
  json batchState = json::object();
  double batchPolled = 0;
  std::string batchMessage;
  bool showTransform = false;
  float transformScale = 1.f;
  int transformUnits = 0, transformOrigin = 0;
  vec3 transformRotate{0.f};
  bool transformGround = false;
  std::string modelExportDir, modelExportName;
  int modelExportFormat = 0, modelExportSize = 0;  // glb gltf obj; 0 = texture set size, else 512..4096
  std::string modelExportStatus;
  float meshFixWeld = 0.f;     // 0 = automatic
  int meshFixHoleEdges = 64;
  std::string meshFixStatus;
  float symmetryPlaneX = 0.f;
  void drawSymmetryWindow();
  void drawMeshCheckWindow();
  void drawModelExportWindow();
  void drawReduceWindow();
  void drawBatchWindow();
  void drawTransformWindow();
  void drawCollisionWindow();  // 0.34
  void drawLightmapWindow();   // 0.35
  bool showLightmapWindow = false;
  int lightmapSize = 512, lightmapSource = 0;
  float lightmapPadding = 4.f;
  std::string lightmapStatus;
  bool showCollisionWindow = false;
  std::string collisionStatus;
  int modelExportCollision = 0;  // none, unreal, godot, plain
  static std::string describeMeshFix(const json& r);
  // 0.10: auto texture from material slot names
  bool showAutoTexture = false;
  json autoTexGuess;                    // auto_texture apply:false
  std::vector<std::string> autoTexPick;  // per listed slot: material id ("" = skip)
  int autoTexStyle = 0, autoTexMapping = 0;
  void openAutoTexture();
  void drawAutoTextureWindow();
  // 0.11: Patina Assistant (AssistantPanel.cpp)
  struct AssistantUI {
    bool show = false;
    bool focusInput = false;
    std::string request;                    // the text box
    std::string asked;                      // what the plan answers
    json context;                           // assistant_context for `asked`
    std::future<AssistantModel::Answer> pending;
    double askedAt = 0;
    json plan;                              // the answer (null: none yet)
    json checked;                           // assistant_check of the plan (steps in words, warnings)
    std::string answerError;
    double answerSeconds = 0;
    bool previewing = false;                // the plan is applied, waiting for Keep or Cancel
    uint64_t previewSerial = 0;             // its undo entry
    std::string previewError;
    std::vector<std::string> previewNotes;  // what the previewed plan found worth telling
    bool previewWhenReady = false;           // assistant_panel {preview}: preview the plan once it arrives
    std::vector<std::string> history;       // recent requests, newest first (saved with the session)
    std::string feedbackNote;
    bool feedbackOpen = false;
    std::string hoverPart;                  // a part button under the mouse: its faces are shown selected
    std::vector<uint8_t> hoverSaved;        // the selection before
    std::string modelHash;
    struct Copy {                           // Choose model file: copying it into the models folder
      std::atomic<float> progress{0.f};
      std::atomic<bool> done{false};
      std::string error, target, hash;      // read once done
    };
    std::shared_ptr<Copy> copy;             // shared with the copying thread (detached)
  } assistant;
  void openAssistant(const std::string& request = "", bool preview = false);
  void drawAssistantWindow();
  void assistantAsk(const std::string& request);
  void assistantPollAnswer();
  void assistantPreview(const json& plan);
  void assistantCancelPreview();
  void assistantKeep();
  void assistantModelControls();            // also in Preferences > Features
  std::vector<std::string> assistantExamples();
  // 0.10: macro recorder window
  std::string macroName = "my macro";
  json macroSaved;  // macro_list
  void playMacro(const json& args);
  void drawMacroWindow();
  const char* keyLabel(const char* actionId) const;  // for menus
  void initKeymap(KeymapPreset preset = KeymapPreset::Patina);
  void applyKeymapPreset(KeymapPreset preset, bool alsoNavigation);
  void drawPreferences();
  void drawAddLayerMenu();
  // (1.2.7) the shared new-layer list; run does an action now or later
  void newLayerItems(const std::function<void(std::function<void()>)>& run);
  void viewMenuItems();  // (1.2.7) the View menu, in the menu bar and the 3D view's right-click menu
  void smartMaterialItems(const std::function<void(std::function<void()>)>& run);
  void addFillLayerUI(float mask);
  // (0.22.5) drag and drop onto the layer stack (LayersPanel.cpp)
  void dropMaterial(const MaterialRef& m, bool fromLibrary, int target, bool replace, bool into, bool below);
  void stackDropTarget(const ImRect& r, int index);
  void maskDragSource(const Layer& l, unsigned tex);
  void smartMaskTarget(int index);
  std::vector<SmartMaterialInfo> smartList;  // (the Smart materials tab's tiles, read again every few seconds)
  double smartListAt = -1e9;
  // 0.4
  void makeIslandFromSelection();
  // ---- 0.13 retopology (RetopoUI.cpp): patch and density brushes on the model, the Retopology window
  bool showRetopo = true;    // (open: a tab beside Properties unless closed; kept between sessions)
  bool retopoShown = false;  // the window was on screen last frame (not a tab behind another)
  bool retopoFocus = false;  // bring its tab to the front next frame
  // Shift with Lines: the loop a click would draw there, found once the pointer rests
  std::vector<vec3> ringPreview;
  vec2 ringPreviewAt{-1e9f, -1e9f};
  double ringPreviewSince = 0;
  bool ringPreviewDone = false;
  int ringMode = 0;         // (0.22) Shift+click: 0 round the part (else facing you), 1 round the part, 2 facing you
  int ringPreviewUsed = 0;  // what the preview shows: 1 round the part, 2 facing you
  // (0.37) the zones or density and the quads show in the 3D view: the top bar's Retopology button (with the
  // retopo_preview_toggle feature off: while the window is in front, as before 0.37)
  bool retopoPreviewOn() const;
  bool retopoInUse() const { return retopoPreviewOn() || retopoTool(); }
  void retopoShowSettings(bool inBar);  // what the 3D view shows of the retopology (the window's Show, the bar's arrow)
  float retopoDensityValue = 2.f;    // what the density brush paints: a multiplier on the quads per m²
  int retopoSuggest = 40;            // how many patches "Suggest patches" makes
  bool retopoAskUse = false;         // open the "use the quads as the model?" question
  bool retopoKeepLayers = true;      // (1.1) ... with the layers baked down onto them
  bool retopoStrokeActive = false;
  vec2 retopoLast{0};                // where the last frame's dab was
  bool retopoHasLast = false;
  std::vector<float> retopoDensityBefore;
  std::vector<uint32_t> retopoVertAdjStart, retopoVertAdj, retopoStamp;  // the density brush's vertex walk
  uint32_t retopoStampId = 0;
  uint64_t retopoStatsVersion = ~0ull;
  int retopoPatchCount = 0;
  double retopoArea = 0;             // of the model, m² (for the quad estimate)
  void drawRetopoWindow();
  void retopoMenu();
  void retopoContextBar();
  void openRetopo();
  void showRetopoWindow() { showRetopo = true, retopoFocus = true; }
  bool retopoTool() const {
    return tool == ViewportTool::Patch || tool == ViewportTool::Density || tool == ViewportTool::ZoneLine || tool == ViewportTool::Flow;
  }
  std::vector<vec3> retopoStrokePts;   // the Lines or Flow stroke being drawn: on the surface,
  std::vector<vec2> retopoStrokeScreen;  // and where the pen went on screen
  bool retopoSnap(vec2 local, float maxPx, vec3& out);  // the nearest visible point of the zone lines
  uint32_t retopoZoneFace = ~0u;       // where the chosen zone was clicked
  std::vector<std::pair<uint32_t, float>> retopoSeedsBefore;  // zone resolutions when a slider drag began
  // the patch / density brushes: called from drawViewport with the frame's input
  void retopoBrush(vec2 local, bool hovered, bool inside, bool clicked, bool moved);
  void retopoEndStroke();
  void retopoOverlay(ImVec2 origin, ImVec2 avail);  // the stroke being drawn, and where a line would snap
  int retopoHoverEdges = 0;                         // the edge loop under the pointer (Loops): its edges
  void retopoBuild();
  bool retopoMakingQuads();  // a full build is running or waiting (not just the live preview)
  json islandDensityArg() const;
  size_t overlappingIslandCount();
  void drawDensityWindow();
  // 0.11.1: a texture set of their own for the selected faces (head / body ...)
  void drawNewTextureSetWindow();
  void openNewTextureSet();
  void openWindowByName(const std::string& window, const json& args);  // open_window (0.12)
  std::string focusWindowNext;  // a docked window to bring to the front next frame
  bool newSetPack = true;
  char newSetName[64] = "";
  void drawRenderDialog();
  void drawHighPolyDialog();
  void drawWelcome();
  void checkMeshFile();
  void drawFilePrompts();     // mesh changed on disk, crash recovery
  void startRecoveryCheck();  // at start: did the last session end cleanly?
  void endRecoveryCheck();    // clean exit
  void saveIncremental();
  void importMaterialFolder(const std::string& dir);  // 0.5: Substance / packed maps, several texture sets
  std::unique_ptr<MaterialDef> pendingMaterialSave;   // edits from the Materials panel, applied after the grid
  void exportAgain();
  bool runCommand(const char* cmd, const json& args, json* out = nullptr);  // errors -> error dialog
  void loadKeymap();
  void saveKeymap();
  void loadSession();
  void saveSession(bool force);
  void addRecentFile(const std::string& path);
  void noteColorUsed(vec3 c);
  void applyUiScale(float user);
  void groupSelectedIntoFolder();
  bool confirmDiscard();  // returns true if OK to discard unsaved work

  // ---- 0.36: toolbars with icons (Toolbars.cpp) and right-click menus (ContextMenus.cpp)
  bool toolbarNames = false;  // Preferences: the names beside the toolbar icons
  bool toolbarIcons() const { return engine.features.on("toolbar_icons"); }
  bool contextMenus() const { return engine.features.on("context_menus"); }
  // A toolbar button: its icon (icons/<icon>.svg), or its name too (Preferences, or `withName`), or only the
  // name with the toolbar_icons feature off. Hovered: the name, the shortcut of `keyId` and `tip`; right-click:
  // its shortcut. `w` 0: a square, or as wide as the name.
  bool iconButton(const char* id, const char* icon, const char* name, ui::IconState state, float h,
                  const char* tip = nullptr, const char* keyId = nullptr, bool withName = false, float w = 0.f);
  float iconButtonWidth(const char* name, float h, bool withName = false) const;
  void shortcutMenu(const char* keyId, const char* name);  // the right-click menu of a button with a shortcut
  void viewportBar();        // the bar over the 3D view: brush, locks, view, the tool's own settings
  void selectTool(ViewportTool t);
  // translations (LanguageUI.cpp, core/I18n.h)
  void drawLanguagePrefs();
  bool setLanguageUI(const std::string& code);  // the interface in that language (the fonts are made again)
  void followLanguage();                         // each frame: a language set by a command takes effect
  uint64_t languageVersion = 0;
  std::string languageSaved = "en";              // in session.json
  std::vector<i18n::Language> languageList;
  double languageListAt = -1e9;
  std::string newLanguageCode, newLanguageName;
  // right-click menus (ContextMenus.cpp)
  void viewportContextMenu(ImVec2 origin, ImVec2 avail);
  bool viewportMenuOpen = false;   // (the 3D view's menu is up: the view ignores the mouse)
  RayHit viewportMenuHit;          // what was under the pointer when it opened
  vec2 viewportMenuAt{0.f};        // where (viewport pixels)
  std::vector<Engine::LayerHere> layersAtMenu;  // the layers that paint the spot
  bool viewRmbMenuOk = false;      // the right button went down where a click opens the menu
  void colorsContextMenu(int which, vec3 color, int index);  // 0 primary, 1 secondary, 2 swatch, 3 recent
  void historyContextMenu(int index);
  void consoleContextMenu();
  void layersEmptyMenu();
  void materialsEmptyMenu();
  void brushesEmptyMenu();
  bool notesContextMenu(uint32_t id);  // true: delete it

  // panels (Panels.cpp / Viewport3D.cpp / UVEditor.cpp)
  void drawMenuBar();
  void drawToolbar();
  void drawViewport(bool fullscreen = false);
  void drawUVEditor();
  void drawLayers();
  // ---- (1.2) the scene's objects (ObjectsUI.cpp): the Objects list over the layers, the placement, Add a model
  void drawObjectsSection();
  void objectMenu(int i);
  void objectPopups();  // (rename, remove)
  bool objectPlacementUI();
  void selectObjectUI(int i);   // the active object from the next frame on (never in the middle of drawing its layers)
  void addModelToScene(const std::string& path);
  void objectSwitched();        // the window's caches of the object that was active go
  void queueSceneAction(std::function<void()> fn);
  void runSceneActions();       // (the start of a frame)
  std::vector<std::function<void()>> sceneActions;
  int objectRename = -1, objectRemove = -1;
  bool objectRenameOpen = false;
  std::string objectRenameText;
  Document::Placement placementBefore;
  // a model file dropped on the window while a model is open: true when it was taken (added, or asked about)
  bool modelDropped(const std::string& path);
  void drawDropModelPrompt();
  std::string droppedModel;
  int modelExportObjects = 0;
  float selectFlatAngle = 10.f;  // (1.2) Select > The flat region: the most the surface bends from face to face
  // (1.2) the Face Sets panel (FaceSetsUI.cpp)
  void drawFaceSetsWindow();
  void watchBrushFolder();  // (1.2) the brushes folder looked at every few seconds
  void importBrushesUI(const std::vector<std::string>& files);
  void faceSetsPanel();
  void openFaceSets() { showFaceSets = true, faceSetsFocus = true; }
  bool showFaceSets = true, faceSetsFocus = false;
  int faceSetRename = 0;
  bool faceSetRenameOpen = false;
  std::string faceSetRenameText;    // the model export window: 0 every object shown, 1 the active one
  bool exportAllObjects = false;  // the texture export window: every object's maps
  bool dropModelAsk = false;
  // the Object tool (ObjectTool.cpp): gizmoMode 0 move, 1 turn, 2 scale; gizmoDrag the handle held (0..2 an axis, 3
  // the middle or the outer ring; -1 none)
  void objectToolUI(ImVec2 origin, ImVec2 size, bool hovered, vec2 local);
  int gizmoMode = 0, gizmoDrag = -1;
  // (1.2.3) G, R and S in the Object tool: objectModal 0 move, 1 turn, 2 scale (-1 none), on one axis (-1 free), by a
  // number typed or the mouse
  int objectModal = -1, objectModalAxis = -1;
  std::string objectModalTyped;
  Document::Placement objectModalStart;
  ImVec2 objectModalMouse0{0, 0};
  float objectModalAngle = 0.f, objectModalPrevAngle = 0.f;
  int objectModalEndFrame = -1;  // (the Esc that put it back closes no window that frame)
  Document::Placement gizmoStart;
  ImVec2 gizmoMouse0{0, 0}, gizmoCenter0{0, 0};
  float gizmoAngle = 0.f;
  void drawProperties();
  void drawMaterials();
  void drawSmartMaterialTiles();  // (0.22.5)
  void materialViewControls();    // (1.2.3) tiles or list, and the tiles' size
  void materialTileZoom();        // (1.2.3) Ctrl+mouse wheel over the tiles: their size
  void drawSmartMaskTiles();
  void drawBrushes();
  void drawTextureSet();
  void drawConsole();
  void drawBlender();
  void drawExportDialog();
  void drawMaterialEditor();
  void drawHelp();
  void drawAbout();
  void openLocalPage(const std::string& which);  // "manual" or "web"
  void drawStatusBar();
  void drawHistory();
  void drawColors();
  void drawKeymapEditor();
  void drawErrorPopup();
  void drawLayerRow(int index, int depth);
  void layerContextMenu(int index);
  // (0.41) baked map layers: their section of the layer list, rows, menu and properties (BakedLayersUI.cpp)
  void drawBakedSection();
  void drawBakedRow(int index);
  void bakedContextMenu(int index);
  void drawBakedProperties(Layer& l);
  bool bakedMapMenuItems();  // "add a layer" items for baked maps without one; false if there are none
  void bakeMapAgain(int map);
  // the 3D view shows a baked map while its layer is selected, and goes back after
  void followBakedSelection();
  uint32_t bakedFollowUid = 0;
  int viewBeforeBaked = -1;

  // helpers
  void handleDroppedFile(const std::string& path);
  void pollDialogs();
  void setupStyle(float scale, float density);
  void buildDefaultLayout(ImGuiID dockspace);
  void handleShortcuts();
  float currentPressure() const;
  void applyFaceSelectionClick(uint32_t face, bool add, bool remove);
  // (0.45) what a Select click or drag on `face` takes: the face, or its UV island; with Visible only, just the faces
  // of it the camera sees (kept while the view and the island stay the same, for drags)
  std::vector<uint32_t> selectionFacesAt(uint32_t face);
  uint64_t visibleCacheKey = 0;
  std::vector<uint32_t> visibleCache;
  void commitUVTransform();
  void cancelUVTransform();

  // ---- (0.46) the Guided layout (Guided.cpp): the work in five steps (Model, UVs, Bake, Paint, Export), each a
  // screen of its own over the same document, with what that step needs in front. The docked layout of before is
  // the Full layout; either can be chosen at any time (the switch at the right of the menu bar).
  enum class GuidedStep { Model = 0, UVs, Bake, Paint, Export, Count };
  struct StepState {
    int state = 0;  // 0 not yet, 1 done, 2 needs a look
    std::string text;  // why, in a few words
  };
  // (0.47) each step's color: the Guided layout's tabs and its screen's tint, and in both layouts the tools, panels and
  // menus that belong to that step. panelStep: the step of a panel (English name), Count for none.
  static ImVec4 stepColor(GuidedStep s);
  static GuidedStep panelStep(const char* englishName);
  static ImU32 mixColor(ImVec4 a, ImVec4 b, float t, float alpha = 1.f);
  ImVec4 baseColors[ImGuiCol_COUNT];  // the style's colors as setupStyle made them (the tint starts from these)
  ImVec4 tintNow{0, 0, 0, 0};        // the color the screen leans to now (w: how far), eased toward the step's
  float tabLift[5] = {0, 0, 0, 0, 0};
  void applyStepTint();               // each frame, before any window
  int pushPanelColors(const char* label);  // a docked panel's tab in its step's color; returns what to pop
  ImVec4 iconAccent{0, 0, 0, 0};      // the chosen tool's color in iconButton (w 0: the theme's accent)
  // (1.2.7) a third layout, Blender (BlenderLayout.cpp); Full is the docked one
  enum class LayoutMode { Guided = 0, Full, Blender };
  LayoutMode layoutMode = LayoutMode::Full;  // session.json "layout": "guided", "full" or "blender" (new installs: guided)
  bool firstStart = false;    // no session.json yet
  GuidedStep guidedStep = GuidedStep::Model;
  bool guidedOn() const;      // the Guided layout is what shows (its feature on, the panels not hidden)
  bool blenderOn() const;     // the Blender layout is what shows
  void drawGuided();
  void guidedStepBar(float h);
  void setGuidedStep(GuidedStep s);
  void setLayoutMode(LayoutMode m);
  void setLayoutMode(bool guided) { setLayoutMode(guided ? LayoutMode::Guided : LayoutMode::Full); }
  void setThemeUI(ui::ThemeId theme);  // (1.2.7) Edit > Preferences > Display > Theme: the style made again before the next frame
  StepState guidedStatus(GuidedStep s);
  void guidedModelPanel();
  void guidedUVPanel();
  void guidedBakePanel();
  void guidedPaintPanel(ImVec2 size);
  void guidedExportPanel();
  void guidedNextButton();
  void layoutSwitch();        // Guided | Full | Blender, at the right of the menu bar
  json layoutCommand(const json& args);  // the `layout` command
  int guidedToolGroups = ~0;  // the tool strip's groups shown (bits: 1 paint, 2 UV, 4 retopology, 8 symmetry, 16 notes, 32 scene)
  bool viewOnly = false;      // the 3D view only looks (Guided: Model, Bake, Export): a left drag orbits, no tool acts
  ViewMode viewBeforeStep = ViewMode::Material;  // the view mode a step changed, given back when it is left
  ViewMode stepViewMode = ViewMode::Material;
  bool stepChangedView = false;
  void stepView(ViewMode m);  // a step shows a view mode (a bake, the checker): given back when the step is left
  ViewportTool guidedLastTool = ViewportTool::Paint;
  bool toolPicked = false;    // selectTool() was called: a tool of another step goes to that step
  int guidedPaintTab = -1;    // open_window: the Paint step's side tab to bring to the front (0 brush, 1 colors, 2 history)
  // a panel drawn as a region of the guided screen instead of its docked window (see beginPanel)
  bool panelEmbedded = false;
  ImVec2 panelEmbedSize{0, 0};
  std::vector<std::pair<std::string, ImVec4>> embeddedRects;  // window.<name> of the regions this frame
  bool beginPanel(const char* label, ImGuiWindowFlags flags = 0);
  void endPanel();
  void embedPanel(void (App::*draw)(), ImVec2 pos, ImVec2 size);
  // the pointer is over the panel `name` (English): its docked window (`exact`: not a window inside it), or its region of
  // the Guided layout
  bool panelHovered(const char* name, bool exact) const;
  // (0.46) a project with unsaved work is never replaced without asking: New, Open, Open Recent, the sample,
  // a dropped file (Save / Don't save / Cancel)
  std::function<void()> pendingReplace;
  std::string pendingReplaceWhat;
  void replaceProject(const std::string& what, std::function<void()> action);
  void drawReplacePrompt();
  std::function<void()> afterSave;  // runs once Save As has written the project (a replace waiting for it)
  bool replaceAsked = false;        // the prompt is to open this frame
  // shared by the panels and the Guided steps
  void textureSetupUI();            // resolution and texture sets (Texture Set panel, the Model step)
  void baseMaterialUI();            // the material under every layer (Texture Set panel, the Paint step's Scene tab)
  void lightingUI();                // the 3D view's lighting (the same)
  void sceneUI() { baseMaterialUI(), lightingUI(); }
  bool exportTexturesNow();         // the Export Textures dialog's settings, written now (false: an error was shown)
  ExportOptions exportOptions();    // (0.47) the Export Textures dialog's settings
  void exportPlanUI(int maxRows);   // the files they would write (0: all)
  bool bakeWant[3] = {true, true, true};  // the Bake step's maps: ambient occlusion, curvature, thickness

  // ---- (1.2.7) the Blender layout (BlenderLayout.cpp): a screen of its own over the same panels, arranged as Blender
  // arranges its editors. The steps are workspace tabs in the menu bar; the tools stand at the 3D view's left and its
  // sidebar (Brush, Colors, View, Blender: vertical tabs) at its right; the Outliner (objects, their texture sets and
  // layers) and the Properties editor (vertical icon tabs) are at the right; the asset shelf (materials, smart
  // materials, smart masks and brushes, by category) is under the 3D view in the Paint step, the UV editor beside it
  // in the UVs step. The areas' borders are dragged to resize them.
  enum class PropsTab { Step = 0, Object, Layer, TextureSet, FaceSets, Retopology, History, Notes, Console, Count };
  enum class SideTab { Brush = 0, Colors, View, Blender, Count };
  enum class ShelfKind { Materials = 0, SmartMaterials, SmartMasks, Brushes };
  struct BlenderUI {
    float rightW = 420.f, outlinerH = 560.f, shelfH = 230.f, sidebarW = 310.f;  // (at UI scale 1)
    float uvShare = 0.45f;          // the UV editor's part of the width beside the 3D view (UVs step)
    PropsTab props = PropsTab::Step;
    SideTab side = SideTab::Brush;
    bool sideOpen = true;
    ShelfKind shelf = ShelfKind::Materials;
    std::string brushCategory;      // the shelf's brush category ("": all)
    std::string outlinerFilter;     // the Outliner's search: layers whose name has it
    bool setsOpen = true, layersOpen = true;  // the Outliner's texture sets and layers, open
  } blui;
  bool outlinerDrawing = false;     // layersTree draws the Outliner's layers (its search applies)
  void drawBlenderLayout();
  void blenderStepTabs();           // the steps as workspace tabs, in the menu bar
  void setBlenderStep(GuidedStep s);
  void drawOutliner();
  void drawPropertiesEditor();
  void drawViewSidebar();
  void drawAssetShelf();
  bool blenderOpenWindow(const std::string& window);  // open_window in the Blender layout: its tab (false: not one)
  void blenderSessionLoad(const json& j);
  json blenderSessionSave() const;
  // the parts of the Layers panel the Outliner and the Properties editor take apart (LayersPanel.cpp)
  void layersButtons();
  void layersTree(float height);    // (height 0: as tall as the stack, up to the space there is)
  void layerProperties();
  int materialsTabShown = -1;       // drawMaterials: the shelf chose this tab and the category (-1: its own tabs)
  std::string brushCategoryShown;   // drawBrushes: only this category ("": every one; the shelf sets it)
  bool brushCategoryOnly = false;
  void retopoPanel();               // the Retopology window's content (RetopoUI.cpp)
  void stepGuidePanel();            // the step's own panel, the Properties editor's Step tab (Guided.cpp)
  void blenderFollowTool();         // a tool of another step brings its workspace up (Guided.cpp)
};

}  // namespace patina
