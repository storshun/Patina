#include "engine/AssistantModel.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>

#include "core/Log.h"

#ifdef PATINA_HAS_ASSISTANT
#include "llama.h"
#endif

namespace patina {

std::string assistantPrompt(const std::string& system, const std::string& user) {
  return "<|im_start|>system\n" + system + "<|im_end|>\n<|im_start|>user\n" + user +
         "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";
}

namespace {
double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

struct AssistantModel::Impl {
  AssistantModel* self = nullptr;
  std::thread worker;
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  bool quit = false;
  mutable std::mutex infoMutex;
  std::string error, path, backend;
  bool onGpu = false;
  std::atomic<bool> cancelFlag{false};
  double deadline = 0;
#ifdef PATINA_HAS_ASSISTANT
  llama_model* model = nullptr;
  llama_context* ctx = nullptr;
  std::vector<llama_token> cached;  // what the KV cache holds, in order
#endif

  void post(std::function<void()> f) {
    {
      std::lock_guard<std::mutex> lk(m);
      jobs.push_back(std::move(f));
    }
    cv.notify_one();
  }
  void run() {
    for (;;) {
      std::function<void()> f;
      {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return quit || !jobs.empty(); });
        if (quit && jobs.empty()) break;
        f = std::move(jobs.front());
        jobs.pop_front();
      }
      f();
    }
    freeModel();
  }
  void setError(const std::string& e) {
    std::lock_guard<std::mutex> lk(infoMutex);
    error = e;
  }
  void freeModel() {
#ifdef PATINA_HAS_ASSISTANT
    if (ctx) llama_free(ctx);
    if (model) llama_model_free(model);
    ctx = nullptr;
    model = nullptr;
    cached.clear();
#endif
  }

#ifdef PATINA_HAS_ASSISTANT
  static bool abortCb(void* data) {
    auto* d = (Impl*)data;
    return d->cancelFlag.load() || (d->deadline > 0 && now() > d->deadline);
  }

  void doLoad(const std::string& file, int gpuLayers) {
    freeModel();
    self->state_ = State::Loading;
    static bool backendReady = false;
    if (!backendReady) {
      llama_log_set([](ggml_log_level level, const char* text, void*) {
        if (level == GGML_LOG_LEVEL_ERROR) PLOG_W("llama.cpp: %s", text);
      }, nullptr);
      llama_backend_init();
      backendReady = true;
    }
    double t0 = now();
    llama_model_params mp = llama_model_default_params();
    bool gpu = llama_supports_gpu_offload();
    mp.n_gpu_layers = gpu ? (gpuLayers < 0 ? 999 : gpuLayers) : 0;
    model = llama_model_load_from_file(file.c_str(), mp);
    if (!model) {
      setError("could not load the model file " + file);
      self->state_ = State::Failed;
      return;
    }
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 4096;
    cp.n_batch = 2048;
    cp.n_ubatch = 512;
    int threads = std::clamp((int)std::thread::hardware_concurrency() / 2, 1, 16);
    if (const char* env = std::getenv("PATINA_ASSISTANT_THREADS"); env && std::atoi(env) > 0) threads = std::atoi(env);
    cp.n_threads = threads;
    cp.n_threads_batch = threads;
    cp.no_perf = true;
    ctx = llama_init_from_model(model, cp);
    if (!ctx) {
      setError("could not start the model (out of memory?)");
      freeModel();
      self->state_ = State::Failed;
      return;
    }
    llama_set_abort_callback(ctx, &Impl::abortCb, this);
    {
      std::lock_guard<std::mutex> lk(infoMutex);
      path = file;
      onGpu = gpu && mp.n_gpu_layers > 0;
      backend = onGpu ? "GPU" : "CPU, " + std::to_string(threads) + " threads";
      error.clear();
    }
    PLOG_I("Assistant model loaded in %.1f s (%s): %s", now() - t0, backend.c_str(), file.c_str());
    self->state_ = State::Ready;
  }

  Answer doAsk(const std::string& prompt, const std::string& grammar, double timeout, int maxTokens) {
    Answer a;
    double t0 = now();
    if (!model || !ctx) {
      a.error = "no model is loaded";
      return a;
    }
    const llama_vocab* vocab = llama_model_get_vocab(model);
    std::vector<llama_token> toks(prompt.size() + 16);
    int n = llama_tokenize(vocab, prompt.c_str(), (int)prompt.size(), toks.data(), (int)toks.size(), false, true);
    if (n < 0) {
      toks.resize(-n);
      n = llama_tokenize(vocab, prompt.c_str(), (int)prompt.size(), toks.data(), (int)toks.size(), false, true);
    }
    toks.resize(std::max(0, n));
    a.promptTokens = (int)toks.size();
    if ((int)toks.size() + maxTokens > (int)llama_n_ctx(ctx)) {
      a.error = "the project description is too long for the model";
      return a;
    }
    // reuse what the cache already holds of this prompt (the system prompt, most of the project)
    size_t p = 0;
    while (p < cached.size() && p < toks.size() && cached[p] == toks[p]) ++p;
    if (p == toks.size()) --p;  // decode at least one token, for its logits
    llama_memory_seq_rm(llama_get_memory(ctx), 0, (llama_pos)p, -1);
    cached.resize(p);
    a.reusedTokens = (int)p;
    cancelFlag = false;
    deadline = t0 + timeout;
    for (size_t i = p; i < toks.size();) {
      int k = (int)std::min<size_t>(2048, toks.size() - i);
      if (llama_decode(ctx, llama_batch_get_one(toks.data() + i, k)) != 0) {
        cached.clear();
        llama_memory_clear(llama_get_memory(ctx), true);
        a.error = cancelFlag ? "cancelled" : now() > deadline ? "timed out" : "the model failed to read the request";
        return a;
      }
      cached.insert(cached.end(), toks.begin() + i, toks.begin() + i + k);
      i += k;
    }
    llama_sampler* gram = nullptr;
    if (!grammar.empty()) {
      gram = llama_sampler_init_grammar(vocab, grammar.c_str(), "root");
      if (!gram) {
        a.error = "the plan grammar could not be read";
        return a;
      }
    }
    const int nVocab = llama_vocab_n_tokens(vocab);
    std::vector<llama_token_data> all;
    std::string out;
    for (int i = 0; i < maxTokens; ++i) {
      if (cancelFlag || now() > deadline) {
        a.error = cancelFlag ? "cancelled" : "timed out";
        break;
      }
      // greedy: the most likely token; the grammar is asked about that one token first, and only
      // when it says no is it run over the whole vocabulary (the model mostly writes valid plans)
      const float* logits = llama_get_logits_ith(ctx, -1);
      llama_token t = (llama_token)(std::max_element(logits, logits + nVocab) - logits);
      if (gram) {
        llama_token_data one = {t, logits[t], 0.f};
        llama_token_data_array arr = {&one, 1, -1, false};
        llama_sampler_apply(gram, &arr);
        if (!std::isfinite(one.logit)) {
          all.resize(nVocab);
          for (int k = 0; k < nVocab; ++k) all[k] = {k, logits[k], 0.f};
          llama_token_data_array full = {all.data(), all.size(), -1, false};
          llama_sampler_apply(gram, &full);
          float best = -INFINITY;
          for (size_t k = 0; k < full.size; ++k)
            if (full.data[k].logit > best) best = full.data[k].logit, t = full.data[k].id;
          if (!std::isfinite(best)) {
            a.error = "the model could not continue the plan";
            break;
          }
        }
        llama_sampler_accept(gram, t);
      }
      if (llama_vocab_is_eog(vocab, t)) {
        a.ok = true;
        break;
      }
      char buf[256];
      int len = llama_token_to_piece(vocab, t, buf, sizeof buf, 0, false);
      if (len > 0) out.append(buf, len);
      ++a.outputTokens;
      if (llama_decode(ctx, llama_batch_get_one(&t, 1)) != 0) {
        a.error = cancelFlag ? "cancelled" : now() > deadline ? "timed out" : "the model stopped";
        cached.clear();
        llama_memory_clear(llama_get_memory(ctx), true);
        break;
      }
      cached.push_back(t);
    }
    if (gram) llama_sampler_free(gram);
    if (!a.ok && a.error.empty()) a.error = "the plan was longer than the limit";
    a.text = out;
    a.seconds = now() - t0;
    deadline = 0;
    return a;
  }
#endif
};

AssistantModel::AssistantModel() : d_(std::make_unique<Impl>()) {
  d_->self = this;
  d_->worker = std::thread([this] { d_->run(); });
}

AssistantModel::~AssistantModel() {
  d_->cancelFlag = true;
  {
    std::lock_guard<std::mutex> lk(d_->m);
    d_->quit = true;
  }
  d_->cv.notify_one();
  if (d_->worker.joinable()) d_->worker.join();
}

void AssistantModel::load(const std::string& path, int gpuLayers) {
#ifdef PATINA_HAS_ASSISTANT
  state_ = State::Loading;
  d_->post([this, path, gpuLayers] { d_->doLoad(path, gpuLayers); });
#else
  (void)path, (void)gpuLayers;
  d_->setError("this build of Patina has no Assistant");
  state_ = State::Failed;
#endif
}

std::string AssistantModel::error() const {
  std::lock_guard<std::mutex> lk(d_->infoMutex);
  return d_->error;
}
std::string AssistantModel::path() const {
  std::lock_guard<std::mutex> lk(d_->infoMutex);
  return d_->path;
}
std::string AssistantModel::backendInfo() const {
  std::lock_guard<std::mutex> lk(d_->infoMutex);
  return d_->backend;
}
bool AssistantModel::gpu() const {
  std::lock_guard<std::mutex> lk(d_->infoMutex);
  return d_->onGpu;
}

std::future<AssistantModel::Answer> AssistantModel::ask(const std::string& system, const std::string& user,
                                                         const std::string& grammar, double timeoutSeconds, int maxTokens) {
  auto pr = std::make_shared<std::promise<Answer>>();
  auto fut = pr->get_future();
#ifdef PATINA_HAS_ASSISTANT
  std::string prompt = assistantPrompt(system, user);
  d_->post([this, pr, prompt, grammar, timeoutSeconds, maxTokens] {
    if (state_ != State::Ready) {
      Answer a;
      a.error = state_ == State::Loading ? "the model is still loading" : "no model is loaded";
      pr->set_value(a);
      return;
    }
    state_ = State::Busy;
    Answer a = d_->doAsk(prompt, grammar, timeoutSeconds, maxTokens);
    state_ = State::Ready;
    pr->set_value(a);
  });
#else
  (void)system, (void)user, (void)grammar, (void)timeoutSeconds, (void)maxTokens;
  Answer a;
  a.error = "this build of Patina has no Assistant";
  pr->set_value(a);
#endif
  return fut;
}

void AssistantModel::cancel() { d_->cancelFlag = true; }

void AssistantModel::unload() {
  d_->post([this] {
    d_->freeModel();
    state_ = State::NoModel;
  });
}

}  // namespace patina
