#include "ui/App.h"
#include "core/TextRender.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <portable-file-dialogs.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <stb_image_resize2.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "core/Image.h"
#include "AppArgs.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/UVIsland.h"
#include "ui/UiIdCheck.h"
#include "gfx/GL.h"

// generated from packaging/icons/patina_128.png (CMakeLists.txt)
extern const uint8_t kPatinaWindowIcon[];
extern const size_t kPatinaWindowIconSize;

namespace patina {


namespace fs = std::filesystem;

int runGui(const Args& args) {
  App app;
  return app.run(args);
}

// (0.31) What opening a model brought in of its own textures: said in the status line after the "Opened" message,
// and textures the model names but that were not found in a message of their own.
void App::noteModelTextures() {
  const json& r = engine.modelTextures;
  if (!r.is_object() || r.value("path", "") != engine.doc.meshPath) return;
  int n = r.value("textures", 0), slots = r.value("slots", 0);
  char sl[64], tx[64];
  std::snprintf(sl, sizeof sl, P_("%d slot", "%d slots", slots), slots);
  std::snprintf(tx, sizeof tx, P_("%d texture", "%d textures", n), n);
  // ("From the model" is the layer's name: data, not translated)
  std::string s = r.value("vertex_colors", false)
                      ? F_("Brought in the model's materials (%s, %s, vertex colors) as the layer %s", sl, tx, "From the model")
                      : F_("Brought in the model's materials (%s, %s) as the layer %s", sl, tx, "From the model");
  if (r.value("resolution", 0) != r.value("resolution_before", 0))
    s = F_("%s; the texture set is now %d to match them", s.c_str(), r.value("resolution", 0));
  deferredStatus = s;
  std::string lost;
  for (auto* key : {"missing", "unreadable"})
    for (auto& m : r.value(key, json::array())) lost += "\n" + m.get<std::string>();
  if (!lost.empty())
    showError(T_("Some of the model's textures are missing"),
              F_("These were not found beside the model (or in a textures folder) or could not be read, so their slots "
                 "show the material's color instead:%s",
                 lost.c_str()));
}

void App::setStatus(const std::string& s) {
  statusText = s;
  statusTime = nowSeconds();
  PLOG_I("%s", s.c_str());
}

static bool pfdAvailable() {
  static int avail = -1;
  if (avail < 0) avail = pfd::settings::available() ? 1 : 0;
  return avail == 1;
}

void App::openFileDialog(DialogPurpose purpose) {
  if (!pfdAvailable()) {
    setStatus(T_("No file dialog helper found (install zenity or kdialog) - drag & drop files onto the window instead"));
    return;
  }
  dialogPurpose = purpose;
  std::string home = paths::homeDir().string();
  // Windows' save dialog won't open at all when the starting file name has a '/' in it (C:\Users\x/untitled.patina):
  // every starting path uses the system's own separators
  auto native = [](const std::string& p) { return p.empty() ? p : fs::path(p).make_preferred().string(); };
  auto homeFile = [&](const char* name) { return native((fs::path(home) / name).string()); };
  switch (purpose) {
    case DialogPurpose::OpenFile:
      openDialog = std::make_unique<pfd::open_file>(
          T_("Open mesh or project"), home,
          std::vector<std::string>{T_("Meshes & projects"), "*.obj *.gltf *.glb *.fbx *.stl *.3mf *.ply *.ptmesh *.patina", T_("All files"), "*"});
      break;
    case DialogPurpose::LoadHDR:
      openDialog = std::make_unique<pfd::open_file>(T_("Load HDR environment"), home,
                                                    std::vector<std::string>{T_("HDR images"), "*.hdr", T_("All files"), "*"});
      break;
    case DialogPurpose::HighPoly:
      openDialog = std::make_unique<pfd::open_file>(T_("High-poly mesh"), home,
                                                    std::vector<std::string>{T_("Meshes"), "*.obj *.gltf *.glb *.fbx *.stl *.3mf *.ply *.ptmesh", T_("All files"), "*"});
      break;
    case DialogPurpose::ImportBrushes:
      openDialog = std::make_unique<pfd::open_file>(T_("Import brushes"), home,
                                                    std::vector<std::string>{T_("Brush images and Photoshop brushes"), "*.png *.jpg *.jpeg *.tga *.bmp *.gif *.psd *.exr *.abr", T_("All files"), "*"},
                                                    pfd::opt::multiselect);
      break;
    case DialogPurpose::AddObject:
      openDialog = std::make_unique<pfd::open_file>(T_("Add a model to the scene"), home,
                                                    std::vector<std::string>{T_("Meshes"), "*.obj *.gltf *.glb *.fbx *.stl *.3mf *.ply *.ptmesh", T_("All files"), "*"},
                                                    pfd::opt::multiselect);
      break;
    case DialogPurpose::BakeDownLow:
      openDialog = std::make_unique<pfd::open_file>(T_("Low-poly model"), home,
                                                    std::vector<std::string>{T_("Meshes"), "*.obj *.gltf *.glb *.fbx *.stl *.3mf *.ply *.ptmesh", T_("All files"), "*"});
      break;
    case DialogPurpose::RenderPath:
      saveDialog = std::make_unique<pfd::save_file>(T_("Render image"), renderPath.empty() ? homeFile("render.png") : native(renderPath),
                                                    std::vector<std::string>{T_("PNG image"), "*.png"});
      break;
    case DialogPurpose::BrushImage:
      openDialog = std::make_unique<pfd::open_file>(T_("Brush alpha image"), home,
                                                    std::vector<std::string>{T_("Images"), "*.png *.jpg *.tga", T_("All files"), "*"});
      break;
    case DialogPurpose::StencilImage:
      openDialog = std::make_unique<pfd::open_file>(T_("Stencil image"), home,
                                                    std::vector<std::string>{T_("Images"), "*.png *.jpg *.jpeg *.tga *.bmp *.exr", T_("All files"), "*"});
      break;
    case DialogPurpose::FontFile:
      openDialog = std::make_unique<pfd::open_file>(T_("Font"), home,
                                                    std::vector<std::string>{T_("Fonts"), "*.ttf *.otf *.ttc", T_("All files"), "*"});
      break;
    case DialogPurpose::StencilSave:
      saveDialog = std::make_unique<pfd::save_file>(T_("Save the stencil image"), homeFile("stencil.png"),
                                                    std::vector<std::string>{T_("PNG image"), "*.png"});
      break;
    case DialogPurpose::SaveProject:
      saveDialog = std::make_unique<pfd::save_file>(
          T_("Save project"), engine.doc.projectPath.empty() ? homeFile("untitled.patina") : native(engine.doc.projectPath),
          std::vector<std::string>{T_("Patina project"), "*.patina"});
      break;
    case DialogPurpose::ImportMaterial:
      folderDialog = std::make_unique<pfd::select_folder>(T_("Import material folder (PBR maps)"), home);
      break;
    case DialogPurpose::ExportDir:
      folderDialog = std::make_unique<pfd::select_folder>(T_("Export folder"), native(exportDir.empty() ? home : exportDir));
      break;
    case DialogPurpose::BatchFiles:
      openDialog = std::make_unique<pfd::open_file>(
          T_("Models to process"), home,
          std::vector<std::string>{T_("Models"), "*.glb *.gltf *.obj *.fbx *.stl *.3mf *.ply *.ptmesh *.patina", T_("All files"), "*"},
          pfd::opt::multiselect);
      break;
    case DialogPurpose::BatchFolder:
      folderDialog = std::make_unique<pfd::select_folder>(T_("A folder of models"), home);
      break;
    case DialogPurpose::BatchOutput:
      folderDialog = std::make_unique<pfd::select_folder>(T_("Where the results go"), native(batchOutput.empty() ? home : batchOutput));
      break;
    case DialogPurpose::ModelExportDir:
      folderDialog = std::make_unique<pfd::select_folder>(T_("Export folder"), native(modelExportDir.empty() ? home : modelExportDir));
      break;
    case DialogPurpose::RetopoExport: {
      std::string base = engine.doc.meshPath.empty() ? homeFile("retopo.obj")
                                                     : native((fs::path(engine.doc.meshPath).parent_path() /
                                                               (fs::path(engine.doc.meshPath).stem().string() + "_retopo.obj")).string());
      saveDialog = std::make_unique<pfd::save_file>(T_("Export the quads"), base, std::vector<std::string>{T_("Wavefront OBJ"), "*.obj"});
      break;
    }
    default: break;
  }
}

void App::pollDialogs() {
  std::string err;
  if (openDialog && openDialog->ready(0)) {
    auto r = openDialog->result();
    openDialog.reset();
    if (!r.empty()) {
      if (dialogPurpose == DialogPurpose::OpenFile) {
        if (engine.openFile(r[0], &err)) setStatus(F_("Opened %s", r[0].c_str()));
        else showError(F_("Could not open %s", fs::path(r[0]).filename().string().c_str()), err);
      } else if (dialogPurpose == DialogPurpose::LoadHDR) {
        if (engine.env.loadHDR(r[0])) setStatus(F_("Environment: %s", r[0].c_str()));
      } else if (dialogPurpose == DialogPurpose::BrushImage) {
        engine.painter.brush.alpha.image = r[0];
      } else if (dialogPurpose == DialogPurpose::HighPoly) {
        highPolyInput = r[0];
      } else if (dialogPurpose == DialogPurpose::ImportBrushes) {
        importBrushesUI(r);
      } else if (dialogPurpose == DialogPurpose::AddObject) {
        for (auto& f : r) addModelToScene(f);
      } else if (dialogPurpose == DialogPurpose::BakeDownLow) {
        bakeDownInput = r[0];
        bakeDownSource = 0;
      } else if (dialogPurpose == DialogPurpose::StencilImage) {
        Stencil& st = engine.painter.stencil;
        if (st.loadFile(r[0], &err)) {
          st.enabled = true;
          setStatus(F_("Stencil: %s - hold %s to move it", fs::path(r[0]).filename().string().c_str(),
                       std::string(keyLabel("stencil.edit")).c_str()));
        } else {
          showError(T_("Could not load the stencil"), err);
        }
      } else if (dialogPurpose == DialogPurpose::BatchFiles) {
        for (auto& f : r) batchPaths.push_back(f);
      } else if (dialogPurpose == DialogPurpose::FontFile) {
        stencilTextStyle.font = r[0];
        Stencil& st = engine.painter.stencil;
        if (st.setText(stencilText, stencilTextStyle, &err)) st.enabled = true;
        else showError(T_("Could not use the font"), err);
      }
    }
  }
  if (saveDialog && saveDialog->ready(0)) {
    std::string r = saveDialog->result();
    saveDialog.reset();
    if (!r.empty() && dialogPurpose == DialogPurpose::RenderPath) {
      renderPath = r;
    } else if (!r.empty() && dialogPurpose == DialogPurpose::RetopoExport) {
      if (fs::path(r).extension() != ".obj") r += ".obj";
      json out;
      if (runCommand("retopo_export", {{"path", r}}, &out)) setStatus(F_("Exported %d quads to %s", out.value("quads", 0), r.c_str()));
    } else if (!r.empty() && dialogPurpose == DialogPurpose::StencilSave) {
      if (fs::path(r).extension() != ".png") r += ".png";
      if (saveImage(r, engine.painter.stencil.image)) setStatus(F_("Saved %s", r.c_str()));
      else showError(T_("Could not save the stencil image"), r);
    } else if (!r.empty()) {
      if (fs::path(r).extension() != ".patina") r += ".patina";
      if (engine.saveScene(r, &err)) {
        setStatus(F_("Saved %s", r.c_str())), addRecentFile(r);
        if (auto act = std::move(afterSave)) afterSave = nullptr, act();  // (0.46) what waited for the save
      } else {
        showError(T_("The project was NOT saved"), err.empty() ? F_("Could not write %s", r.c_str()) : err);
      }
    }
    if (dialogPurpose == DialogPurpose::SaveProject) afterSave = nullptr;  // (cancelled, or done)
  }
  if (folderDialog && folderDialog->ready(0)) {
    std::string r = folderDialog->result();
    folderDialog.reset();
    if (!r.empty()) {
      if (dialogPurpose == DialogPurpose::ImportMaterial) {
        importMaterialFolder(r);
      } else if (dialogPurpose == DialogPurpose::ExportDir) {
        exportDir = r;
      } else if (dialogPurpose == DialogPurpose::BatchFolder) {
        batchPaths.push_back(r);
      } else if (dialogPurpose == DialogPurpose::BatchOutput) {
        batchOutput = r;
      } else if (dialogPurpose == DialogPurpose::ModelExportDir) {
        modelExportDir = r;
      }
    }
  }
}

void App::saveProject(bool saveAs) {
  if (saveAs || engine.doc.projectPath.empty()) {
    openFileDialog(DialogPurpose::SaveProject);
    return;
  }
  std::string err;
  if (engine.saveScene(engine.doc.projectPath, &err)) setStatus(F_("Saved %s", engine.doc.projectPath.c_str()));
  else showError(T_("The project was NOT saved"), err.empty() ? F_("Could not write %s", engine.doc.projectPath.c_str()) : err);
}

void App::doUndo() {
  if (engine.painter.active()) return;
  if (uvXform.active) {
    cancelUVTransform();
    return;
  }
  std::string label = engine.doc.undo.undoLabel();
  if (engine.doc.undoStep()) setStatus(F_("Undo: %s", label.c_str()));
}

void App::doRedo() {
  if (engine.painter.active()) return;
  std::string label = engine.doc.undo.redoLabel();
  if (engine.doc.redoStep()) setStatus(F_("Redo: %s", label.c_str()));
}

void App::handleDroppedFile(const std::string& path) {
  std::error_code ec;
  std::string err;
  if (fs::is_directory(path, ec)) {
    importMaterialFolder(path);
    return;
  }
  std::string ext = fs::path(path).extension().string();
  for (auto& c : ext) c = (char)tolower(c);
  std::string why = MaterialLibrary::unsupportedMaterialReason(path);
  if (!why.empty()) {
    showError(F_("Patina can't open %s", fs::path(path).filename().string().c_str()), why);
    return;
  }
  if (ext == ".exr") {
    // a single EXR: a texture map of a material, so import its folder
    importMaterialFolder(fs::path(path).parent_path().string());
    return;
  }
  if (ext == ".abr") {  // (1.2) Photoshop brushes: a brush for each tip
    importBrushesUI({path});
    return;
  }
  if (ext == ".hdr") {
    if (engine.env.loadHDR(path)) setStatus(F_("Environment: %s", path.c_str()));
    return;
  }
  if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
    // dropped on the Brushes panel: brush tip; anywhere else: image material (decal)
    // (the Brushes panel, or a window inside it; in the Guided layout its region)
    if (panelHovered("Brushes", false)) {
      importBrushesUI({path});  // (1.2) a brush of its own, kept in the brushes folder
    } else if (auto* e = engine.materials.importImageFile(path, &err)) {
      engine.selectMaterial(e->def.id);
      engine.painter.material.mapping = 2;  // project it from the view like a decal
      setStatus(F_("Image material '%s' (screen projection) - drop on Brushes for a brush tip instead", e->def.name.c_str()));
    } else {
      setStatus(err);
    }
    return;
  }
  // (1.2) a model dropped while one is open: added to the scene, or opened instead (asked)
  if (ext != ".patina" && modelDropped(path)) return;
  replaceProject(T_("open another file"), [this, path] {
    std::string err;
    if (engine.openFile(path, &err)) setStatus(F_("Opened %s", path.c_str()));
    else showError(F_("Could not open %s", fs::path(path).filename().string().c_str()), err);
  });
}

float App::currentPressure() const {
  // the pen's pressure (through the pressure curve) when the last input came from the
  // pen, 1.0 for the mouse
  return pen.pressure();
}

void App::setupStyle(float scale, float density) {
  ImGuiStyle& s = ImGui::GetStyle();
  s = ImGuiStyle();  // start from defaults so a UI-scale change does not compound
  ImGui::StyleColorsDark();
  s.WindowRounding = 3;
  s.FrameRounding = 3;
  s.GrabRounding = 3;
  s.TabRounding = 3;
  s.ScrollbarRounding = 3;
  s.WindowBorderSize = 1;
  s.FrameBorderSize = 0;
  s.WindowPadding = ImVec2(8, 8);
  s.FramePadding = ImVec2(6, 4);
  s.ItemSpacing = ImVec2(6, 5);
  s.IndentSpacing = 14;
  s.ScrollbarSize = 13;
  s.GrabMinSize = 9;
  ImVec4* c = s.Colors;
  auto rgb = [](int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); };
  ImVec4 accent = rgb(232, 133, 44);
  c[ImGuiCol_WindowBg] = rgb(33, 34, 37);
  c[ImGuiCol_ChildBg] = rgb(30, 31, 34);
  c[ImGuiCol_PopupBg] = rgb(28, 29, 32, 0.98f);
  c[ImGuiCol_Border] = rgb(52, 54, 58);
  c[ImGuiCol_FrameBg] = rgb(46, 48, 52);
  c[ImGuiCol_FrameBgHovered] = rgb(58, 60, 66);
  c[ImGuiCol_FrameBgActive] = rgb(66, 68, 74);
  c[ImGuiCol_TitleBg] = rgb(26, 27, 29);
  c[ImGuiCol_TitleBgActive] = rgb(30, 31, 34);
  c[ImGuiCol_MenuBarBg] = rgb(26, 27, 29);
  c[ImGuiCol_ScrollbarBg] = rgb(28, 29, 31);
  c[ImGuiCol_CheckMark] = accent;
  c[ImGuiCol_SliderGrab] = rgb(200, 112, 36);
  c[ImGuiCol_SliderGrabActive] = accent;
  c[ImGuiCol_Button] = rgb(52, 54, 59);
  c[ImGuiCol_ButtonHovered] = rgb(68, 70, 77);
  c[ImGuiCol_ButtonActive] = rgb(200, 112, 36, 0.8f);
  c[ImGuiCol_Header] = rgb(60, 62, 68);
  c[ImGuiCol_HeaderHovered] = rgb(72, 74, 81);
  c[ImGuiCol_HeaderActive] = rgb(200, 112, 36, 0.7f);
  c[ImGuiCol_Separator] = rgb(52, 54, 58);
  c[ImGuiCol_Tab] = rgb(36, 37, 40);
  c[ImGuiCol_TabHovered] = rgb(72, 74, 81);
  c[ImGuiCol_TabSelected] = rgb(50, 52, 57);
  c[ImGuiCol_TabSelectedOverline] = accent;
  c[ImGuiCol_TabDimmed] = rgb(30, 31, 34);
  c[ImGuiCol_TabDimmedSelected] = rgb(42, 44, 48);
  c[ImGuiCol_DockingPreview] = rgb(232, 133, 44, 0.5f);
  c[ImGuiCol_TextSelectedBg] = rgb(232, 133, 44, 0.35f);
  c[ImGuiCol_NavHighlight] = accent;
  c[ImGuiCol_ResizeGripHovered] = accent;
  s.TabBarOverlineSize = 2.f;  // (0.47: the line over a panel's chosen tab, in its step's color)
  s.ScaleAllSizes(scale);
  std::copy(std::begin(s.Colors), std::end(s.Colors), std::begin(baseColors));  // (the steps' tint starts from these)

  ImGuiIO& io = ImGui::GetIO();
  // (0.36) the glyphs: Latin with its accents, typographic punctuation, and every character the translation in
  // use has (the font atlas holds only these)
  static ImVector<ImWchar> ranges;  // (kept for the atlas)
  {
    ImFontGlyphRangesBuilder b;
    b.AddRanges(io.Fonts->GetGlyphRangesDefault());
    static const ImWchar more[] = {0x0100, 0x024F, 0x2010, 0x2027, 0x2030, 0x205E, 0x20AC, 0x20AC, 0x2122, 0x2122, 0};
    b.AddRanges(more);
    const std::string& g = i18n::stats().glyphs;
    if (!g.empty()) b.AddText(g.c_str());
    ranges.clear();
    b.BuildRanges(&ranges);
  }
  std::vector<std::string> fonts = {
#ifdef _WIN32
      "C:/Windows/Fonts/segoeui.ttf",
#endif
      "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf",
      "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/abattis-cantarell-vf-fonts/Cantarell-VF.otf",
      "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      (paths::dataDir() / "fonts" / "DejaVuSans.ttf").string()};
  const float px = 15.f * scale * density;
  std::string mainFont;
  for (const std::string& f : fonts) {
    std::error_code ec;
    if (fs::exists(f, ec) && fontCoverage(f, {'A'}) > 0 && io.Fonts->AddFontFromFileTTF(f.c_str(), px, nullptr, ranges.Data)) {
      mainFont = f;
      break;
    }
  }
  if (mainFont.empty()) {
    ImFontConfig cfg;
    cfg.SizePixels = 13.f * scale * density;
    io.Fonts->AddFontDefault(&cfg);
  } else if (!i18n::stats().glyphs.empty()) {
    // characters the interface font lacks (Chinese, Japanese, Korean...) from a font that has them: the one the
    // translation names first, then the system's
    std::vector<uint32_t> want;
    for (uint32_t cp : utf8Codepoints(i18n::stats().glyphs))
      if (cp > 0x7F) want.push_back(cp);
    std::vector<uint8_t> has;
    fontCoverage(mainFont, want, 0, &has);
    std::vector<uint32_t> missing;
    for (size_t i = 0; i < want.size(); ++i)
      if (!has[i]) missing.push_back(want[i]);
    std::vector<std::string> fallbacks;
    if (!i18n::stats().font.empty()) fallbacks.push_back(i18n::stats().font);
    for (const char* f : {
#ifdef _WIN32
             "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/YuGothM.ttc", "C:/Windows/Fonts/meiryo.ttc",
             "C:/Windows/Fonts/malgun.ttf", "C:/Windows/Fonts/msjh.ttc", "C:/Windows/Fonts/seguisym.ttf",
#endif
             "/usr/share/fonts/google-droid-sans-fonts/DroidSansFallbackFull.ttf",
             "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
             "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
             "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"})
      fallbacks.push_back(f);
    for (const std::string& f : fallbacks) {
      if (missing.empty()) break;
      std::error_code ec;
      if (!fs::exists(f, ec) || fontCoverage(f, missing, 0, &has) <= 0) continue;  // (unusable fonts are left out)
      ImFontConfig cfg;
      cfg.MergeMode = true;
      if (io.Fonts->AddFontFromFileTTF(f.c_str(), px, &cfg, ranges.Data)) {
        std::vector<uint32_t> still;
        for (size_t i = 0; i < missing.size(); ++i)
          if (!has[i]) still.push_back(missing[i]);
        missing.swap(still);
      }
    }
    if (!missing.empty())
      PLOG_W("%zu characters of the translation are in none of the fonts: name one that has them (X-Patina-Font)", missing.size());
  }
  io.FontGlobalScale = 1.f / density;
}

void App::buildDefaultLayout(ImGuiID ds) {
  ImGui::DockBuilderRemoveNode(ds);
  ImGui::DockBuilderAddNode(ds, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(ds, ImGui::GetMainViewport()->WorkSize);
  ImGuiID left, center, right, bottom, rightBottom, centerRight;
  ImGui::DockBuilderSplitNode(ds, ImGuiDir_Left, 0.045f, &left, &center);
  ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, &right, &center);
  ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.27f, &bottom, &center);
  ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.42f, &centerRight, &center);
  ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.55f, &rightBottom, &right);
  ImGui::DockBuilderDockWindow("Tools", left);
  ImGui::DockBuilderDockWindow("Viewport", center);
  ImGui::DockBuilderDockWindow("UV Editor", centerRight);
  ImGui::DockBuilderDockWindow("Layers", right);
  // the window docked last becomes the visible tab
  ImGui::DockBuilderDockWindow("Blender Link", rightBottom);
  ImGui::DockBuilderDockWindow("Colors", rightBottom);
  ImGui::DockBuilderDockWindow("Texture Set", rightBottom);
  ImGui::DockBuilderDockWindow("Retopology", rightBottom);
  ImGui::DockBuilderDockWindow("Face Sets", rightBottom);
  ImGui::DockBuilderDockWindow("Properties", rightBottom);
  // (0.46) the library first, the console last; History and Notes down here too, so the tabs at the right have room
  // for their names
  ImGui::DockBuilderDockWindow("Materials", bottom);
  ImGui::DockBuilderDockWindow("Brushes", bottom);
  ImGui::DockBuilderDockWindow("History", bottom);
  ImGui::DockBuilderDockWindow("Notes", bottom);
  ImGui::DockBuilderDockWindow("Console", bottom);
  if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(left)) n->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
  ImGui::DockBuilderFinish(ds);
  // (Properties in front there: the tab of the window drawn last would be, and Retopology is drawn after it)
  if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(rightBottom)) n->SelectedTabId = ImHashStr("#TAB", 0, ImHashStr("Properties"));
  if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(bottom)) n->SelectedTabId = ImHashStr("#TAB", 0, ImHashStr("Materials"));
  // (the windows appearing for the first time take the keyboard focus, the Console last, and a node shows the focused
  // window's tab: once they are there, Materials and Properties are brought to the front, see run())
  frontTabsIn = 3;
}

int App::run(const Args& args) {
  SDL_SetHint(SDL_HINT_APP_ID, "patina");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    PLOG_E("SDL_Init failed: %s", SDL_GetError());
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  window = SDL_CreateWindow("Patina", 1600, 960, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    PLOG_E("SDL_CreateWindow failed: %s", SDL_GetError());
    return 1;
  }
  {  // window icon (the taskbar on X11 and Windows; Wayland desktops take it from patina.desktop)
    Image icon;
    if (decodeImage(kPatinaWindowIcon, kPatinaWindowIconSize, icon, 4)) {
      std::vector<uint8_t> px(icon.pixels.size());
      for (size_t i = 0; i < px.size(); ++i) px[i] = (uint8_t)std::lround(std::clamp(icon.pixels[i], 0.f, 1.f) * 255.f);
      if (SDL_Surface* s = SDL_CreateSurfaceFrom(icon.width, icon.height, SDL_PIXELFORMAT_RGBA32, px.data(), icon.width * 4)) {
        SDL_SetWindowIcon(window, s);
        SDL_DestroySurface(s);
      }
    }
  }
  SDL_GLContext ctx = SDL_GL_CreateContext(window);
  if (!ctx) {
    PLOG_E("OpenGL 4.5 context creation failed: %s", SDL_GetError());
    return 1;
  }
  SDL_GL_MakeCurrent(window, ctx);
  SDL_GL_SetSwapInterval(1);
  if (!gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress) || !GLAD_GL_VERSION_4_5) {
    PLOG_E("OpenGL 4.5 is required");
    return 1;
  }
  PLOG_I("OpenGL: %s", gl::rendererString().c_str());
  SDL_MaximizeWindow(window);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
  // (the folder made first: ImGui saves the layout there every few seconds and says nothing when it can't)
  static std::string iniPath = (paths::ensureDir(paths::configDir()) / "layout.ini").string();
  bool firstRun = !fs::exists(iniPath);
  io.IniFilename = iniPath.c_str();
  // Wayland reports pixel density (framebuffer px per window unit) separately from
  // the content scale; X11 HiDPI reports only a content scale. Sizes follow the
  // content scale, fonts are rasterized at full pixel resolution.
  float displayScale = SDL_GetWindowDisplayScale(window);
  float density = SDL_GetWindowPixelDensity(window);
  if (displayScale <= 0) displayScale = 1.f;
  if (density <= 0) density = 1.f;
  baseUiScale = std::max(0.5f, displayScale / density);
  pixelDensity = density;
  loadKeymap();
  uiScale = baseUiScale;
  setupStyle(uiScale, density);
  ImGui_ImplSDL3_InitForOpenGL(window, ctx);
  ImGui_ImplOpenGL3_Init("#version 450 core");

  engine.defaultResolution = args.resolution;
  engine.autosaveMinutes = 5;
  engine.lastAutosave = nowSeconds();
  engine.init(false);
  loadSession();  // brush, material, colors, recent files, UI scale, navigation, language
  if (std::abs(userUiScale - 1.f) > 0.01f || i18n::current() != "en") pendingUiScale = userUiScale;
  languageVersion = i18n::version();
  startRecoveryCheck();
  // (0.46) in the Guided layout the Model step is the start: the welcome window only the first time (its choices)
  showWelcome = showWelcomeOnStart && args.file.empty() && !recoveryPending &&
                (!guidedLayout || firstStart || !engine.features.on("guided_layout"));
  std::string err;
  if (!args.file.empty() && !engine.openFile(args.file, &err)) setStatus(F_("Could not open %s: %s", args.file.c_str(), err.c_str()));

  // command server (Blender add-on, MCP)
  if (args.server) {
    auto handler = [this](const json& req) {
      auto f = engine.post(req);
      if (f.wait_for(std::chrono::seconds(300)) != std::future_status::ready)
        return json{{"ok", false}, {"error", "timed out"}};
      return f.get();
    };
    for (int p = args.port; p < args.port + 10; ++p) {
      if (server.start(p, handler, &err)) {
        serverPort = server.port();
        engine.serverPort = serverPort;
        if (p != args.port) setStatus(F_("Port %d busy - command server on %d", args.port, p));
        break;
      }
    }
    if (!serverPort) setStatus(F_("Command server could not start: %s", err.c_str()));
  }
  registerPenCommands();
  registerPieCommands();
  // HTTP API: command-line options win over the Preferences setting
  launchArgs = args;
  if (args.httpGiven || httpEnabled) startHttpApi(!args.httpGiven);
  engine.openAssistantPanel = [this](const std::string& request, bool preview) { openAssistant(request, preview); };
  engine.openWindow = [this](const std::string& w, const json& a) { openWindowByName(w, a); };
  engine.layoutHook = [this](const json& a) { return layoutCommand(a); };
  engine.requestWindowCapture = [this](std::shared_ptr<std::promise<json>> p, json a) {
    if (capturePromise)  // never leave an earlier request hanging
      capturePromise->set_value({{"ok", false}, {"error", "superseded by a newer screenshot request"}});
    capturePromise = std::move(p);
    captureArgs = std::move(a);
  };
  engine.onExternalChange = [this](const std::string& what) {
    if (what == "bridge")
      setStatus(engine.doc.uvsCarried >= 0.f
                    ? F_("Received '%s' from Blender without its UVs: kept Patina's", engine.doc.bridge.object.c_str())
                    : F_("Received '%s' from Blender", engine.doc.bridge.object.c_str()));
    if (what == "bridge_project") {  // (0.44) the project the object was painted in
      addRecentFile(engine.doc.projectPath);
      setStatus(F_("Received '%s' from Blender: opened its project %s", engine.doc.bridge.object.c_str(),
                   fs::path(engine.doc.projectPath).filename().string().c_str()));
    }
    if (what == "mesh") noteModelTextures();
    if (what == "object" || what == "objects" || what == "project" || what == "mesh") objectSwitched();
  };
  engine.recentFiles = [this] { return recentFiles; };
  engine.requestFrameWait = [this](std::shared_ptr<std::promise<json>> p, int frames) {
    frameWaits.push_back({std::move(p), frames});
  };

  bool quit = false;
  bool quitRequested = false;
  frameStats.init();
  ui::idCheckInit();
  while (!quit) {
    frameStats.beginFrame();
    if (!deferredStatus.empty()) setStatus(deferredStatus), deferredStatus.clear();
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      // Tab toggles the distraction-free view; ImGui would otherwise also use it to move focus
      if ((ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) && ev.key.key == SDLK_TAB &&
          !(ev.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_GUI)) && !ImGui::GetIO().WantTextInput &&
          keymapCapture.empty()) {
        bool tabIsFocus = false;
        for (auto& a : keymap) tabIsFocus |= !std::strcmp(a.id, "view.focus") && a.chord == ImGuiKey_Tab;
        if (tabIsFocus) {
          if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) distractionFree = !distractionFree, distractionFreeSince = nowSeconds();
          continue;
        }
      }
      if (!pen.handleEvent(ev)) continue;  // a pen button mapped to a Patina action
      ImGui_ImplSDL3_ProcessEvent(&ev);
      switch (ev.type) {
        case SDL_EVENT_QUIT: quitRequested = true; break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
          if (ev.window.windowID == SDL_GetWindowID(window)) quitRequested = true;
          break;
        case SDL_EVENT_DROP_FILE:
          if (ev.drop.data) handleDroppedFile(ev.drop.data);
          break;
        default: break;
      }
    }
    pen.beginFrame();
    pen.pumpInjected(window);  // test harness only (inject_pen)
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
      engine.tick();
      if (capturePromise) {
        capturePromise->set_value({{"ok", false}, {"error", "the Patina window is minimized - use render_view instead"}});
        capturePromise.reset();
      }
      for (auto& w : frameWaits) w.first->set_value({{"ok", true}, {"result", {{"frames", "minimized"}}}});
      frameWaits.clear();
      SDL_Delay(30);
      continue;
    }
    engine.viewportSize = lastViewportSize;
    frameStats.mark("events");
    engine.tick();
    frameStats.mark("tick");
    pollDialogs();
    runSceneActions();  // (1.2) another object made active, removed, copied
    watchBrushFolder();
    if (!engine.features.on("translations") && i18n::current() != "en") i18n::setLanguage("en");
    followLanguage();  // (0.36) a language chosen by a command (language_set) or switched off
    if (pendingUiScale > 0.f) {
      // rebuild style and font atlas between frames
      uiScale = baseUiScale * pendingUiScale;
      pendingUiScale = 0.f;
      io.Fonts->Clear();
      setupStyle(uiScale, pixelDensity);
      ImGui_ImplOpenGL3_DestroyFontsTexture();
      ImGui_ImplOpenGL3_CreateFontsTexture();
    }
    {
      // remember opened / saved files (also when opened by MCP or Blender)
      const std::string& f = engine.doc.projectPath.empty() ? engine.doc.meshPath : engine.doc.projectPath;
      if (f != lastSeenFile) {
        lastSeenFile = f;
        if (!f.empty()) addRecentFile(f);
      }
    }
    {
      static std::string lastTitle;
      std::string title = std::string("Patina - ") +
                          (engine.doc.projectPath.empty() ? std::string(T_("untitled")) : fs::path(engine.doc.projectPath).filename().string()) +
                          (engine.sceneDirty() ? " *" : "") +
                          (engine.doc.bridge.active ? "  [Blender: " + engine.doc.bridge.object + "]" : "");
      if (title != lastTitle) SDL_SetWindowTitle(window, title.c_str()), lastTitle = title;
    }

    frameStats.mark("dialogs");
    // (0.22.2) a line, flow stroke or density stroke being drawn: everything else waits for it
    engine.drawingStroke = retopoStrokeActive && (tool == ViewportTool::ZoneLine || tool == ViewportTool::Flow || tool == ViewportTool::Density);
    uv::uvHashHold(engine.drawingStroke);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    applyStepTint();  // (0.47) the Guided layout's screen in its step's color
    ui::valueEntryNewFrame(engine.features.on("slider_typing"));  // (0.42) right-click a slider to type its value
    frameStats.mark("newframe");
    uv::uvHashNewFrame();  // (the UV fingerprint is worked out at most once a frame)
    forcedAction = pendingAction;  // a palette pick runs this frame as if its shortcut was pressed
    {
      // texture set tools: switched off, they stop acting; isolation follows the texture set shown
      bool setTools = engine.features.on("texture_set_tools");
      if (!setTools) engine.viewOptions.isolateSet = engine.painter.lockSet = false;
      if (!engine.features.on("symmetry_check")) {
        if (engine.viewOptions.mode == ViewMode::Symmetry) engine.viewOptions.mode = ViewMode::Material;
        if (engine.compare.view == ViewMode::Symmetry) engine.compare.view = ViewMode::Material;
      }
      engine.viewOptions.isolateTile = engine.uvOptions.tile;
      engine.painter.isolateTile =
          engine.viewOptions.isolateSet && engine.doc.mesh.tiles.active() ? engine.uvOptions.tile : -1;
    }
    pendingAction.clear();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    // menu bar + status bar reserve space
    drawMenuBar();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - ImGui::GetFrameHeight()));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                 ImGuiWindowFlags_NoBackground;
    // (0.37) with the panels hidden the host is empty but in front of the view: it must not take the mouse,
    // or the view gets no strokes, navigation or gizmo clicks
    // (0.46) the same with the Guided layout, whose screen is under it
    const bool guided = guidedOn();
    if (distractionFree || guided) hostFlags |= ImGuiWindowFlags_NoMouseInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##DockHost", nullptr, hostFlags);
    ImGui::PopStyleVar(2);
    ImGuiID ds = ImGui::GetID("MainDock");
    if (firstRun || requestLayoutReset || !ImGui::DockBuilderGetNode(ds)) {
      buildDefaultLayout(ds);
      firstRun = false;
      requestLayoutReset = false;
    }
    // (0.46) the Guided layout draws its own screen: the docked layout stays as it was, out of sight
    embeddedRects.clear();
    if (!guided) viewOnly = false;
    ImGui::DockSpace(ds, ImVec2(0, 0), distractionFree || guided ? ImGuiDockNodeFlags_KeepAliveOnly : ImGuiDockNodeFlags_None);
    ImGui::End();

    followBakedSelection();  // (0.41) a baked map layer selected: the 3D view shows its map
    if (distractionFree) {
      drawViewport(true);
    } else if (guided) {
      drawGuided();
      frameStats.mark("panels");
    } else {
      drawToolbar();
      frameStats.mark("dock");
      drawViewport();
      frameStats.mark("viewport");
      drawUVEditor();
      frameStats.mark("uv");
      drawLayers();
      frameStats.mark("layers");
      // (0.46) a dock node's tabs are in the order their windows are drawn (the one in front: buildDefaultLayout)
      drawBlender();
      drawColors();
      drawTextureSet();
      drawProperties();
      frameStats.mark("panels");
      drawMaterials();
      frameStats.mark("materials");
      drawBrushes();
      drawHistory();
      drawNotes();
      drawConsole();
      if (frontTabsIn > 0 && --frontTabsIn == 0) {
        // (a dock node shows the tab of the window with the keyboard focus, and the window that appears last takes
        // it: the Console) the focus to the 3D view, then Materials and Properties in front of their nodes
        ImGui::SetWindowFocus("Viewport");
        for (const char* name : {"Materials", "Properties"})
          if (ImGuiWindow* w = ImGui::FindWindowByName(name); w && w->DockNode && w->DockNode->TabBar)
            w->DockNode->TabBar->NextSelectedTabId = w->TabId;
      }
    }
    drawExportDialog();
    drawMaterialEditor();
    drawHelp();
    drawAbout();
    drawKeymapEditor();
    drawPreferences();
    drawAddLayerMenu();
    drawDensityWindow();
    drawNewTextureSetWindow();
    if (!focusWindowNext.empty()) {  // open_window for a docked panel (Texture Set, Blender Link)
      ImGui::SetWindowFocus(focusWindowNext.c_str());
      focusWindowNext.clear();
    }
    drawSymmetryWindow();
    drawMeshCheckWindow();
    drawModelExportWindow();
    drawReduceWindow();
    drawBatchWindow();
    drawTransformWindow();
    drawCollisionWindow();
    drawLightmapWindow();
    drawAutoTextureWindow();
    drawMacroWindow();
    drawAssistantWindow();
    frameStats.mark("windows");
    if (!guidedOn()) drawRetopoWindow();  // (a docked panel: in the Full layout)
    if (!guidedOn()) drawFaceSetsWindow();  // (1.2)
    frameStats.mark("retopo");
    drawRenderDialog();
    drawHighPolyDialog();
    drawBakeDownDialog();
    drawWelcome();
    drawPenTest();
    drawFilePrompts();
    checkMeshFile();
    drawErrorPopup();
    drawReplacePrompt();
    drawDropModelPrompt();  // (1.2)
    drawStatusBar();
    drawCommandPalette();
    // (0.36) where the windows and the open popup are, for tests and the manual's screenshots (window pixels; a
    // window by its English name)
    {
      for (auto it = engine.uiRects.begin(); it != engine.uiRects.end();)
        it = it->first.rfind("window.", 0) == 0 || it->first == "popup.top" ? engine.uiRects.erase(it) : std::next(it);
      ImGuiContext& g = *GImGui;
      for (ImGuiWindow* win : g.Windows) {
        // (docked windows count, though they are child windows of the dock host)
        if (!win->Active || (win->Flags & ImGuiWindowFlags_Tooltip) || ((win->Flags & ImGuiWindowFlags_ChildWindow) && !win->DockIsActive))
          continue;
        const char* n = win->Name;
        if (const char* p = std::strstr(n, "###")) n = p + 3;
        engine.uiRects[std::string("window.") + n] = {win->Pos.x, win->Pos.y, win->Size.x, win->Size.y};
      }
      for (auto& [n, r] : embeddedRects) engine.uiRects[n] = {r.x, r.y, r.z, r.w};  // (the Guided layout's regions)
      if (!g.OpenPopupStack.empty())
        if (ImGuiWindow* w = g.OpenPopupStack.back().Window)
          engine.uiRects["popup.top"] = {w->Pos.x, w->Pos.y, w->Size.x, w->Size.y};
    }
    handleShortcuts();
    forcedAction.clear();
    // a finished slider drag / edit becomes one undo step (see Document::checkpoint)
    engine.uiInteracting = ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                           ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    saveSession(false);
    if (showDemo) ImGui::ShowDemoWindow(&showDemo);

    if (quitRequested) {
      if (!engine.sceneDirty()) quit = true;
      else ImGui::OpenPopup("Unsaved changes");
      quitRequested = false;
    }
    if (ImGui::BeginPopupModal(L_("Unsaved changes"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted(T_("The project has unsaved changes."));
      if (ImGui::Button(L_("Save"))) {
        if (!engine.doc.projectPath.empty()) {
          saveProject(false);
          quit = true;
        } else {
          saveProject(true);
        }
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button(L_("Quit without saving"))) quit = true;
      ImGui::SameLine();
      if (ImGui::Button(L_("Cancel"))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    frameStats.mark("rest");
    ImGui::Render();
    ui::idCheckEndFrame();
    if (ui::idCheckConflicts().size() != engine.uiIdConflicts.size()) engine.uiIdConflicts = ui::idCheckConflicts();
    int fbw, fbh;
    SDL_GetWindowSizeInPixels(window, &fbw, &fbh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, fbw, fbh);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.1f, 0.1f, 0.11f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (capturePromise) {
      // read the back buffer before presenting
      std::vector<uint8_t> px(size_t(fbw) * fbh * 4);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadBuffer(GL_BACK);
      glReadPixels(0, 0, fbw, fbh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      Image img;
      img.resize(fbw, fbh, 3);
      for (int y = 0; y < fbh; ++y)
        for (int x = 0; x < fbw; ++x)
          for (int c = 0; c < 3; ++c) img.at(x, y)[c] = px[(size_t(fbh - 1 - y) * fbw + x) * 4 + c] / 255.f;
      int maxW = std::max(64, captureArgs.value("max_width", 1600));
      if (img.width > maxW) {
        Image small;
        int nh = std::max(1, img.height * maxW / img.width);
        small.resize(maxW, nh, 3);
        stbir_resize_float_linear(img.pixels.data(), img.width, img.height, 0, small.pixels.data(), maxW, nh, 0,
                                  STBIR_RGB);
        img = std::move(small);
      }
      auto png = encodePng(img);
      capturePromise->set_value({{"ok", true},
                                 {"result", {{"image_png_base64", base64Encode(png.data(), png.size())},
                                             {"width", img.width},
                                             {"height", img.height}}}});
      capturePromise.reset();
    }
    frameStats.mark("imgui_render");
    SDL_GL_SwapWindow(window);
    frameStats.mark("swap");
    // (0.45) wait_frames: tests wait for frames, not for fixed times
    for (auto it = frameWaits.begin(); it != frameWaits.end();) {
      if (--it->second > 0) {
        ++it;
        continue;
      }
      it->first->set_value({{"ok", true}, {"result", {{"frames", "drawn"}}}});
      it = frameWaits.erase(it);
    }
    frameStats.endFrame();
  }

  saveSession(true);
  endRecoveryCheck();
  httpApi.stop();
  server.stop();
  engine.shutdown();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DestroyContext(ctx);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

}  // namespace patina
