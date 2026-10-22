// Patina Assistant (0.11): the window. Type a request, see the plan in plain words, preview it in
// the viewport (as one undo step), adjust the numbers that matter, keep it or cancel it.
#include <cctype>
#include <cstring>
#include <filesystem>
#include <set>
#include <fstream>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Sha256.h"
#include "engine/Assistant.h"
#include "engine/AssistantContext.h"
#include "portable-file-dialogs.h"
#include "ui/App.h"

namespace patina {

namespace fs = std::filesystem;

namespace {

// Where the model file is published: empty until it is (an open decision; the manual says where).
const char* kModelUrl = "";

// A part as a person names it: slot:M_Jacket -> "jacket", group:Arm.L -> "left arm",
// slot:BootsLeather -> "boots leather", region:head -> "the head"
std::string partWord(const std::string& id) {
  auto c = id.find(':');
  std::string name = c == std::string::npos ? id : id.substr(c + 1);
  if (id.rfind("region:", 0) == 0) return "the " + name;
  if (id.rfind("piece:", 0) == 0) return "piece " + name;
  for (const char* pre : {"M_", "MI_", "MAT_", "Mat_", "mat_"})
    if (name.rfind(pre, 0) == 0 && name.size() > strlen(pre)) name = name.substr(strlen(pre));
  std::string side;
  for (const char* s : {".L", "_L", ".R", "_R"})
    if (name.size() > 2 && name.compare(name.size() - 2, 2, s) == 0) {
      side = s[1] == 'L' ? "left " : "right ";
      name.resize(name.size() - 2);
    }
  std::string out;
  for (size_t i = 0; i < name.size(); ++i) {
    char ch = name[i];
    if (ch == '_' || ch == '.' || ch == '-') ch = ' ';
    else if (i && std::isupper((unsigned char)ch) && std::islower((unsigned char)name[i - 1])) out += ' ';
    out += (char)std::tolower((unsigned char)ch);
  }
  return side + out;
}

// the steps' numbers worth a slider: strength (opacity), the color of a tint or of a colored
// material, and the one value of an adjustment layer
struct Tweak {
  int step;
  std::string key;       // "strength", "opacity", or "adjust.<name>"
  std::string colorKey;  // "color" (tint), "tint" (material), or empty
  float lo = 0.f, hi = 1.f, def = 0.5f;
  std::string label;     // "Strength", "Brightness"... (English: T_() where it is shown)
};
std::vector<Tweak> tweaksOf(const json& plan) {
  std::vector<Tweak> out;
  auto steps = plan.value("steps", json::array());
  for (size_t i = 0; i < steps.size(); ++i) {
    std::string cmd = steps[i].value("cmd", std::string());
    const json& a = steps[i].value("args", json::object());
    if (cmd == "add_tint_layer") out.push_back({(int)i, "strength", "color", 0.f, 1.f, 0.5f, N_("Strength")});
    else if (cmd == "add_material_layer")
      out.push_back({(int)i, "strength", a.contains("tint") ? "tint" : "", 0.f, 1.f, 1.f, N_("Strength")});
    else if (cmd == "update_layer" && a.contains("opacity")) out.push_back({(int)i, "opacity", "", 0.f, 1.f, 1.f, N_("Opacity")});
    else if (cmd == "add_layer" && a.value("type", std::string()) == "adjust" && a.contains("adjust") &&
             a["adjust"].is_object() && a["adjust"].size() == 1) {
      std::string k = a["adjust"].begin().key();
      bool offset = k == "roughness" || k == "metallic" || k == "height";
      Tweak t{(int)i, "adjust." + k, "", offset ? -1.f : 0.f, offset ? 1.f : 2.f, offset ? 0.f : 1.f, k};
      t.label[0] = (char)std::toupper((unsigned char)t.label[0]);
      if (k == "hue") t.lo = -0.5f, t.hi = 0.5f, t.def = 0.f;
      out.push_back(t);
    }
  }
  return out;
}

}  // namespace

void App::openAssistant(const std::string& request, bool preview) {
  if (!engine.features.on("patina_assistant")) return;
  assistant.show = true;
  assistant.focusInput = true;
  if (!request.empty()) {
    assistant.request = request;
    assistantAsk(request);
    assistant.previewWhenReady = preview && assistant.pending.valid();
  }
}

std::vector<std::string> App::assistantExamples() {
  // requests built from the open model's own parts
  std::vector<std::string> ex;
  if (!engine.doc.hasMesh()) return ex;
  std::vector<std::string> slots, regions;
  for (auto& p : engine.meshParts().parts) {
    if (p.kind == "slot" || p.kind == "group") slots.push_back(partWord(p.id));
    if (p.kind == "region") regions.push_back(p.id);
  }
  if (!slots.empty()) {
    ex.push_back("Make the creases of the " + slots[0] + " slightly redder");
    if (slots.size() > 1) ex.push_back("Make the " + slots[1] + " worn leather");
    ex.push_back("Add dirt in the crevices of the " + slots.back());
  } else {
    ex.push_back("Add rust in the cavities");
    ex.push_back("Wear the paint off the edges");
  }
  if (std::find(regions.begin(), regions.end(), "region:head") != regions.end())
    ex.push_back("One UV island for the head and another for the body");
  ex.push_back("Dust where it would settle, a bit");
  ex.push_back("Show me the curvature map");
  return ex;
}

void App::assistantAsk(const std::string& request) {
  auto& A = assistant;
  if (request.empty() || A.pending.valid()) return;
  if (A.previewing) assistantKeep();  // a new request: what was previewed stays
  AssistantModel& m = engine.assistantModel();
  std::string file = engine.assistantModelFile();
  if (m.state() == AssistantModel::State::NoModel || m.state() == AssistantModel::State::Failed) {
    if (!fs::exists(file)) {
      A.answerError = T_("There is no model yet: choose the model file below.");
      return;
    }
    m.load(file);
  }
  A.asked = request;
  A.plan = json();
  A.checked = json();
  A.answerError.clear();
  A.previewError.clear();
  A.previewNotes.clear();
  A.history.erase(std::remove(A.history.begin(), A.history.end(), request), A.history.end());
  A.history.insert(A.history.begin(), request);
  if (A.history.size() > 12) A.history.resize(12);
  A.context = buildAssistantContext(engine, request);
  std::string text = A.context["text"];
  while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();
  A.pending = m.ask(A.context["system"], text + "\nRequest: " + request, assistant::buildGrammar(engine));
  A.askedAt = nowSeconds();
}

void App::assistantPollAnswer() {
  auto& A = assistant;
  if (!A.pending.valid() || A.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  AssistantModel::Answer ans = A.pending.get();
  bool wantPreview = A.previewWhenReady;  // once, for this answer only
  A.previewWhenReady = false;
  A.answerSeconds = ans.seconds;
  if (!ans.ok) {
    if (ans.error == "cancelled") A.answerError.clear();
    else if (ans.error == "timed out")
      A.answerError = T_("The Assistant took longer than 30 seconds. The computer may be busy with something else.");
    else
      A.answerError = F_("The Assistant didn't answer: %s", ans.error.c_str());
    return;
  }
  json plan = json::parse(ans.text, nullptr, false);
  if (plan.is_discarded()) {
    A.answerError = T_("The Assistant's answer wasn't a plan.");
    return;
  }
  A.plan = plan;
  runCommand("assistant_check", {{"plan", plan}}, &A.checked);
  // a plan that only changes the view or the brush is harmless: it runs straight away
  bool harmless = !plan.value("steps", json::array()).empty();
  for (auto& st : plan.value("steps", json::array())) {
    static const std::set<std::string> view = {"set_view", "set_camera", "set_brush", "set_tool", "set_paint_material",
                                               "set_paint_channels", "set_symmetry", "set_environment", "set_compare",
                                               "open_window"};
    harmless &= view.count(st.value("cmd", std::string())) > 0;
  }
  if (harmless && A.checked.value("ok", false)) {
    json r;
    runCommand("assistant_apply", {{"plan", plan}, {"request", A.asked}}, &r);
    A.plan = json();
    setStatus(F_("Assistant: %s", plan.value("say", std::string(T_("done"))).c_str()));
  } else if (wantPreview && A.checked.value("ok", false)) {
    assistantPreview(plan);
  }
}

void App::assistantPreview(const json& plan) {
  auto& A = assistant;
  if (A.previewing) assistantCancelPreview();
  A.previewError.clear();
  A.previewNotes.clear();
  json r;
  try {
    r = engine.execute("assistant_apply", {{"plan", plan}, {"request", A.asked}});
  } catch (std::exception& e) {
    A.previewError = e.what();
    return;
  }
  for (auto& n : r.value("notes", json::array()))
    if (n.is_string()) A.previewNotes.push_back(n);
  A.previewing = true;
  A.previewSerial = engine.doc.undo.topSerial();
}

void App::assistantCancelPreview() {
  auto& A = assistant;
  if (!A.previewing) return;
  // only the previewed step, and only while it is still the last thing done
  if (engine.doc.undo.topSerial() == A.previewSerial) {
    engine.doc.undoStep();
    if (!engine.assistantTurns.empty() && engine.assistantTurns.back().serial == A.previewSerial) engine.assistantTurns.pop_back();
  }
  A.previewing = false;
}

void App::assistantKeep() {
  auto& A = assistant;
  A.previewing = false;
  A.plan = json();
  A.request.clear();
  A.focusInput = true;
}

void App::assistantModelControls() {
  auto& A = assistant;
  AssistantModel& m = engine.assistantModel();
  std::string file = engine.assistantModelFile();
  bool exists = fs::exists(file);
  if (A.copy && A.copy->done) {
    if (!A.copy->error.empty()) showError(T_("Could not copy the model file"), A.copy->error);
    else {
      engine.assistantModelPath = A.copy->target;
      A.modelHash = A.copy->hash;
      saveSession(true);
      m.unload();
      m.load(A.copy->target);
      setStatus(F_("Assistant model ready to load: %s", A.copy->target.c_str()));
    }
    A.copy.reset();
  }
  if (A.copy) {
    ImGui::ProgressBar(A.copy->progress.load(), ImVec2(-1, 0), T_("Copying the model file..."));
    return;
  }
  if (!exists) {
    ImGui::TextWrapped("%s", T_("There is no model yet. The Patina Assistant model is a separate download (a .gguf file of "
                                "about 1 GB, see the manual's Assistant page); choose it here and it is copied into Patina's "
                                "own folder."));
    if (*kModelUrl) {
      if (ImGui::SmallButton(L_("Where to get it"))) ImGui::SetClipboardText(kModelUrl), setStatus(F_("Copied: %s", kModelUrl));
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(T_("%s (copied to the clipboard)"), kModelUrl);
      ImGui::SameLine();
    }
  } else {
    static const char* states[] = {N_("not loaded"), N_("loading..."), N_("ready"), N_("thinking..."), N_("failed")};
    const char* state = T_(states[(int)m.state()]);
    if (m.backendInfo().empty())
      ImGui::TextDisabled(T_("Model: %s (%s)"), fs::path(file).filename().string().c_str(), state);
    else
      ImGui::TextDisabled(T_("Model: %s (%s, %s)"), fs::path(file).filename().string().c_str(), state, m.backendInfo().c_str());
    if (m.state() == AssistantModel::State::Failed) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", m.error().c_str());
  }
  if (ImGui::SmallButton(exists ? L_("Choose another model file...") : L_("Choose model file..."))) {
    auto sel = pfd::open_file(T_("Patina Assistant model"), paths::homeDir().string(), {T_("GGUF model"), "*.gguf"}).result();
    if (!sel.empty()) {
      auto job = std::make_shared<AssistantUI::Copy>();
      fs::path target = paths::ensureDir(paths::userDataDir() / "models") / "patina-assistant.gguf";
      job->target = target.string();
      std::string src = sel[0];
      // (translated here: the copy runs on another thread, and translations are looked up on this one)
      std::string readFailed = F_("reading %s failed", src.c_str());
      std::thread([job, src, target, readFailed] {
        std::error_code ec;
        uintmax_t total = fs::file_size(src, ec), done = 0;
        std::ifstream in(src, std::ios::binary);
        fs::path tmp = target.string() + ".part";
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        Sha256 sha;
        std::vector<char> buf(8 << 20);
        while (in && out) {
          in.read(buf.data(), buf.size());
          std::streamsize n = in.gcount();
          if (n <= 0) break;
          out.write(buf.data(), n);
          sha.update(buf.data(), (size_t)n);
          done += (uintmax_t)n;
          job->progress = total ? float(double(done) / double(total)) : 0.f;
        }
        out.close();
        if (!in.eof() || done != total) job->error = readFailed;
        else {
          fs::rename(tmp, target, ec);
          if (ec) job->error = ec.message();
          job->hash = sha.hex();
        }
        job->done = true;
      }).detach();
      A.copy = job;
    }
  }
  if (exists && !A.modelHash.empty()) {
    bool known = false;
    for (auto& k : assistant::knownModels()) known |= A.modelHash == k.sha256;
    if (!known) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%s", T_("(not a model Patina knows: plans may be odd)"));
    }
  }
}

void App::drawAssistantWindow() {
  auto& A = assistant;
  if (!engine.features.on("patina_assistant")) {
    if (A.previewing) assistantKeep();
    A.show = false;
    return;
  }
  assistantPollAnswer();
  if (!A.show) {
    if (A.previewing) assistantKeep();  // closing the window keeps what was previewed
    return;
  }
  ImGui::SetNextWindowSize(ImVec2(420 * uiScale, 520 * uiScale), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(L_("Assistant"), &A.show)) {
    ImGui::End();
    return;
  }
  bool thinking = A.pending.valid();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", T_("Say what you want in plain words. It runs on this computer; nothing is sent anywhere."));
  ImGui::PopStyleColor();
  // ---- the request
  if (A.focusInput) {
    ImGui::SetKeyboardFocusHere();
    A.focusInput = false;
  }
  ImGui::BeginDisabled(thinking);
  ImGui::SetNextItemWidth(-70 * uiScale);
  bool enter = ImGui::InputTextWithHint("##req", "Make the skin's creases slightly redder...", &A.request,
                                        ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  bool askBtn = ImGui::Button(L_("Ask"), ImVec2(-1, 0));
  ImGui::EndDisabled();
  if ((enter || askBtn) && !A.request.empty()) assistantAsk(A.request);
  if (!engine.doc.hasMesh()) ImGui::TextDisabled("%s", T_("Open a model first."));
  // ---- thinking
  if (thinking) {
    float t = float(nowSeconds() - A.askedAt);
    AssistantModel::State st = engine.assistantModel().state();
    std::string what = st == AssistantModel::State::Loading ? F_("Loading the model... %d s", (int)t) : F_("Thinking... %d s", (int)t);
    ImGui::ProgressBar(-1.f * (float)ImGui::GetTime(), ImVec2(-80 * uiScale, 0), what.c_str());
    ImGui::SameLine();
    if (ImGui::Button(L_("Cancel##ask"), ImVec2(-1, 0))) engine.assistantModel().cancel();
  }
  if (!A.answerError.empty()) {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextColored(ImVec4(1, 0.6f, 0.45f, 1), "%s", A.answerError.c_str());
    ImGui::PopTextWrapPos();
    if (!thinking && !A.asked.empty() && ImGui::SmallButton(L_("Try again"))) assistantAsk(A.asked);
  }
  // ---- the answer
  if (A.plan.is_object()) {
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.f);
    if (A.plan.contains("ask")) {
      ImGui::TextUnformatted(A.plan["ask"].get<std::string>().c_str());
      // the parts it can see, as answers; hovering one shows its faces (the selection comes back after).
      // A question that comes with ideas (a vague request) has those as its answers instead.
      ImGui::Spacing();
      std::string hovered;
      json partButtons = A.plan.contains("suggest") ? json::array() : A.context.value("parts", json::array());
      for (auto& p : partButtons) {
        std::string id = p.value("id", std::string());
        ImGui::PushID(id.c_str());
        if (ImGui::SmallButton(partWord(id).c_str())) {
          A.request = A.asked + " (" + id + ")";
          assistantAsk(A.request);
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(T_("%s: %d faces"), p.value("label", std::string()).c_str(), p.value("faces", 0));
          hovered = id;
        }
        ImGui::PopID();
        ImGui::SameLine();
      }
      if (!partButtons.empty()) ImGui::NewLine();
      if (hovered != A.hoverPart) {
        Document& d = engine.doc;
        if (!A.hoverPart.empty() && A.hoverSaved.size() == d.mesh.faceCount()) d.faceSelection = A.hoverSaved;
        A.hoverPart = hovered;
        if (!hovered.empty()) {
          A.hoverSaved = d.faceSelection;
          try {
            auto faces = engine.partFaces(hovered);
            d.faceSelection.assign(d.mesh.faceCount(), 0);
            for (uint32_t f : faces) d.faceSelection[f] = 1;
          } catch (std::exception&) {
          }
        }
        d.updateSelectionFlags();
      }
    } else {
      ImGui::TextUnformatted(A.plan.value("say", std::string()).c_str());
      auto steps = A.checked.value("steps", json::array());
      if (!steps.empty()) {
        for (auto& w : steps) ImGui::BulletText("%s", w.get<std::string>().c_str());
        for (auto& w : A.checked.value("warnings", json::array()))
          ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), T_("Careful: %s"), w.get<std::string>().c_str());
        if (!A.checked.value("ok", false))
          ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), T_("This plan can't run: %s"), A.checked.value("error", std::string()).c_str());
      }
    }
    // 0.12: what it can do instead (or next), as buttons that ask for it
    auto sugg = A.plan.value("suggest", json::array());
    if (sugg.is_array() && !sugg.empty()) {
      ImGui::Spacing();
      ImGui::TextDisabled("%s", A.plan.contains("steps") ? T_("Next, perhaps:") : T_("You could ask:"));
      for (size_t i = 0; i < sugg.size(); ++i) {
        if (!sugg[i].is_string()) continue;
        std::string t = sugg[i];
        ImGui::PushID((int)i + 1000);
        if (ImGui::Button(t.c_str())) {
          A.request = t;
          assistantAsk(t);
        }
        ImGui::PopID();
      }
    }
    ImGui::PopTextWrapPos();
    if (!A.previewError.empty()) {
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), T_("It didn't work: %s"), A.previewError.c_str());
      ImGui::PopTextWrapPos();
    }
    if (A.previewing && !A.previewNotes.empty()) {
      ImGui::PushTextWrapPos(0.f);
      for (auto& n : A.previewNotes) ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%s", n.c_str());
      ImGui::PopTextWrapPos();
    }
    bool runnable = A.plan.contains("steps") && A.checked.value("ok", false);
    if (runnable) {
      // the numbers worth adjusting, straight on the plan; moving one runs the preview again
      bool released = false;
      auto tweaks = tweaksOf(A.plan);
      for (auto& tw : tweaks) {
        json& args = A.plan["steps"][tw.step]["args"];
        ImGui::PushID(tw.step);
        if (!tw.colorKey.empty() && args.contains(tw.colorKey) && args[tw.colorKey].is_array() && args[tw.colorKey].size() == 3) {
          json& col = args[tw.colorKey];
          float c[3] = {col[0], col[1], col[2]};
          if (ImGui::ColorEdit3(L_("Color"), c, ImGuiColorEditFlags_NoInputs)) col = {c[0], c[1], c[2]};
          released |= ImGui::IsItemDeactivatedAfterEdit();
          ImGui::SameLine();
        }
        // several sliders are told apart by the layer they make ("Dirt 0.50", "Dust 0.35")
        std::string label = T_(tw.label.c_str());
        if (tweaks.size() > 1 && args.contains("name") && args["name"].is_string()) label = args["name"].get<std::string>();
        std::string fmtLabel;
        for (char ch : label) fmtLabel += ch == '%' ? std::string("%%") : std::string(1, ch);
        fmtLabel += " %.2f";
        json* target = &args;
        std::string key = tw.key;
        if (key.rfind("adjust.", 0) == 0) target = &args["adjust"], key = key.substr(7);
        float v = target->value(key, tw.def);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##value", &v, tw.lo, tw.hi, fmtLabel.c_str())) (*target)[key] = v;
        released |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::PopID();
      }
      if (released && A.previewing) assistantPreview(A.plan);  // once the slider is let go
      if (assistant::planClearsHistory(A.plan)) {
        // a new texture set, the resolution, the layout: no preview, the undo history goes
        ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%s", T_("This can't be previewed or undone: Patina clears the undo history after it."));
        if (ImGui::Button(L_("Do it"))) {
          json r;
          if (runCommand("assistant_apply", {{"plan", A.plan}, {"request", A.asked}}, &r)) {
            setStatus(F_("Assistant: %s", A.plan.value("say", std::string(T_("done"))).c_str()));
            assistantKeep();
          }
        }
        ImGui::SameLine();
      } else if (!A.previewing) {
        if (ImGui::Button(L_("Preview"))) assistantPreview(A.plan);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Runs the plan so you can see it (one undo step); keep it or cancel it"));
      } else {
        if (ImGui::Button(L_("Keep"))) assistantKeep();
        ImGui::SameLine();
        bool canCancel = engine.doc.undo.topSerial() == A.previewSerial;
        ImGui::BeginDisabled(!canCancel);
        if (ImGui::Button(L_("Cancel"))) assistantCancelPreview();
        ImGui::EndDisabled();
        if (!canCancel && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("%s", T_("You have changed things since: use Undo instead"));
      }
      ImGui::SameLine();
    }
    if (ImGui::Button(L_("Copy as commands"))) {
      json steps = A.plan.value("steps", json::array());
      std::string text;
      for (auto& st : steps) text += st.value("cmd", std::string()) + " " + st.value("args", json::object()).dump() + "\n";
      ImGui::SetClipboardText(text.empty() ? A.plan.dump(2).c_str() : text.c_str());
      setStatus(T_("Copied the plan's commands (paste them into the console)"));
    }
    ImGui::SameLine();
    if (ImGui::Button(L_("Not what I wanted"))) A.feedbackOpen = !A.feedbackOpen;
    if (A.feedbackOpen) {
      ImGui::SetNextItemWidth(-60 * uiScale);
      ImGui::InputTextWithHint("##fb", T_("What did you want? (optional)"), &A.feedbackNote);
      ImGui::SameLine();
      if (ImGui::Button(L_("Save"))) {
        json row = {{"time", (double)std::time(nullptr)}, {"request", A.asked}, {"context", A.context.value("text", std::string())},
                    {"plan", A.plan}, {"wanted", A.feedbackNote}, {"patina", PATINA_VERSION}};
        fs::path f = paths::ensureDir(paths::userDataDir() / "assistant") / "feedback.jsonl";
        std::ofstream(f, std::ios::app) << row.dump() << "\n";
        setStatus(F_("Saved to %s (it stays on this computer)", f.string().c_str()));
        A.feedbackNote.clear();
        A.feedbackOpen = false;
      }
    }
    if (A.answerSeconds > 0) ImGui::TextDisabled(T_("Answered in %.1f s"), A.answerSeconds);
  } else if (!thinking && A.answerError.empty()) {
    // ---- examples from this model's parts, and recent requests
    ImGui::Separator();
    ImGui::TextDisabled("%s", T_("For example:"));
    for (auto& ex : assistantExamples())
      if (ImGui::Selectable(("  " + ex).c_str())) {
        A.request = ex;
        A.focusInput = true;
      }
    // everything it can be asked, by kind, with this model's part names (0.12)
    ImGui::Spacing();
    if (ImGui::TreeNode(L_("Everything you can ask"))) {
      std::string p = "the model", q = "the model";
      bool head = false;
      for (auto& pt : engine.meshParts().parts) {
        if (pt.id == "region:head") head = true;
        if ((pt.kind == "slot" || pt.kind == "group") && p == "the model") p = "the " + partWord(pt.id);
        else if ((pt.kind == "slot" || pt.kind == "group") && q == "the model") q = "the " + partWord(pt.id);
      }
      std::string h = head ? "the head" : p;
      struct Kind {
        const char* title;            // English: L_() where it is shown
        std::vector<std::string> ex;  // requests for the model: English, never translated
      };
      std::vector<Kind> kinds = {
          {N_("Looks"), {"Make " + p + " look battle-worn", "Make it look old and rusty", "Make " + q + " look muddy"}},
          {N_("Materials and colors"), {"Make " + p + " black leather", "Make " + q + " dark green fabric", "Make it brushed steel"}},
          {N_("Wear and dirt"), {"Add dirt in the creases of " + p, "Wear the paint off the edges", "Dust where it would settle, a bit"}},
          {N_("Painting by hand"), {"Set me up to paint scratches on " + p, "Write HERO on " + q, "Paint with gold"}},
          {N_("Adjustments and glow"), {"Make " + p + " a bit darker", "Make " + q + " glossier", "Make " + p + " glow blue", "Make it more metallic"}},
          {N_("Texture sets and texture space"), {"Give " + h + " its own texture set", "Give " + h + " more texture space",
                                                  "Make the textures 2K", "Put " + h + " on a new UDIM tile"}},
          {N_("UVs and bakes"), {"One UV island for " + h, "Pack the UVs", "Fix the overlapping UVs", "Bake ambient occlusion"}},
          {N_("Layers"), {"Turn the dirt down", "Hide the rust", "Make it look clean again", "Rename it to Base"}},
          {N_("Brush, tools and view"), {"Make the brush bigger", "Smooth my strokes", "Show me the roughness", "Turn on symmetry"}},
          {N_("Selections and windows"), {"Select " + h, "Invert the selection", "Export the textures for Unreal", "Render an image of it"}},
          {N_("Questions"), {"How many faces is this?", "Which parts can you see?", "What layers do I have?"}},
      };
      for (auto& k : kinds) {
        if (!ImGui::TreeNode(L_(k.title))) continue;
        for (auto& ex : k.ex)
          if (ImGui::Selectable(("  " + ex).c_str())) {
            A.request = ex;
            A.focusInput = true;
          }
        ImGui::TreePop();
      }
      ImGui::TreePop();
    }
    if (!A.history.empty()) {
      ImGui::Spacing();
      ImGui::TextDisabled("%s", T_("Recent:"));
      for (auto& h : A.history)
        if (ImGui::Selectable(("  " + h).c_str())) {
          A.request = h;
          A.focusInput = true;
        }
    }
  }
  // ---- the model
  ImGui::Spacing();
  ImGui::Separator();
  assistantModelControls();
  ImGui::End();
}

}  // namespace patina
