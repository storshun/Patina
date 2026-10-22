#pragma once
// Patina Assistant (0.11): the model, run with llama.cpp on a worker thread of its own (never the
// GL thread). Loading and generating happen there; results come back through futures or are
// polled by the UI. Generation is greedy (temperature 0) and held to the plan grammar.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace patina {

// The text the model sees, exactly as in training (tools/assistant/train/train_lora.py):
// Qwen3 chat markup with thinking switched off.
std::string assistantPrompt(const std::string& system, const std::string& user);

class AssistantModel {
 public:
  enum class State { NoModel, Loading, Ready, Busy, Failed };
  struct Answer {
    bool ok = false;
    std::string text;     // the plan JSON as written
    std::string error;
    double seconds = 0;   // request to plan
    int promptTokens = 0, reusedTokens = 0, outputTokens = 0;
  };

  AssistantModel();
  ~AssistantModel();

  // Loads a GGUF file (in the background). gpuLayers < 0: every layer on the GPU when there is one.
  void load(const std::string& path, int gpuLayers = -1);
  State state() const { return state_.load(); }
  std::string error() const;
  std::string path() const;
  std::string backendInfo() const;  // e.g. "CUDA0 (all 29 layers)" or "CPU, 8 threads"
  bool gpu() const;

  // Generates a plan for `user` (the project description and the request). The future is ready
  // when the plan is, when it fails, when it is canceled or after `timeoutSeconds`.
  std::future<Answer> ask(const std::string& system, const std::string& user, const std::string& grammar,
                          double timeoutSeconds = 30.0, int maxTokens = 768);
  void cancel();          // stops the generation in progress
  void unload();

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
  std::atomic<State> state_{State::NoModel};
};

}  // namespace patina
