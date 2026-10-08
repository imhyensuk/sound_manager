#include "smix/llm/LocalLlm.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

#if SMIX_HAVE_LLAMA
 #include <llama.h>
#endif

namespace smix::llm
{

#if SMIX_HAVE_LLAMA
struct LocalLlm::Impl
{
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    std::vector<llama_token> cached;          // tokens whose KV is in the context
    mem::BufferId savedState = mem::kNoBuffer; // KV of `savedTokens`, kept while unloaded
    std::vector<llama_token> savedTokens;
    std::uint64_t fileBytes = 0;
};

namespace
{
std::once_flag backendOnce;

void quietLogs()
{
    llama_log_set ([] (ggml_log_level level, const char* text, void*) {
        if (level >= GGML_LOG_LEVEL_ERROR)
            std::fputs (text, stderr);
    }, nullptr);
}
} // namespace
#else
struct LocalLlm::Impl {};
#endif

bool LocalLlm::compiledIn() noexcept
{
#if SMIX_HAVE_LLAMA
    return true;
#else
    return false;
#endif
}

LocalLlm::LocalLlm (mem::Runtime* r) : impl (std::make_unique<Impl>()), runtime (r) {}

LocalLlm::~LocalLlm()
{
    unload();
#if SMIX_HAVE_LLAMA
    if (runtime != nullptr)
        runtime->free (impl->savedState);
#endif
}

bool LocalLlm::isLoaded() const
{
#if SMIX_HAVE_LLAMA
    std::lock_guard<std::mutex> g (lock);
    return impl->ctx != nullptr;
#else
    return false;
#endif
}

std::uint64_t LocalLlm::memoryEstimate() const
{
#if SMIX_HAVE_LLAMA
    // Mapped weights + KV cache (rough: 0.1 MiB per context token for 1-3B models).
    return impl->fileBytes + static_cast<std::uint64_t> (cfg.contextTokens) * 100 * 1024;
#else
    return 0;
#endif
}

bool LocalLlm::load (const Config& c, std::string& error)
{
#if SMIX_HAVE_LLAMA
    std::lock_guard<std::mutex> g (lock);
    if (impl->ctx != nullptr)
        return true;

    std::call_once (backendOnce, [] {
        quietLogs();
        llama_backend_init();
    });

    std::ifstream f (c.modelPath, std::ios::binary | std::ios::ate);
    if (! f)
    {
        error = "모델 파일을 찾을 수 없어요: " + c.modelPath;
        return false;
    }
    impl->fileBytes = static_cast<std::uint64_t> (f.tellg());

    auto mp = llama_model_default_params();
    mp.n_gpu_layers = c.gpuLayers;
    mp.load_mode = LLAMA_LOAD_MODE_MMAP;  // file-backed weights: the OS can drop and re-read them
    impl->model = llama_model_load_from_file (c.modelPath.c_str(), mp);
    if (impl->model == nullptr)
    {
        error = "모델을 불러오지 못했어요 (GGUF 형식인지 확인해 주세요)";
        return false;
    }

    auto cp = llama_context_default_params();
    cp.n_ctx = static_cast<uint32_t> (c.contextTokens);
    cp.n_batch = static_cast<uint32_t> (c.contextTokens);
    const int hw = static_cast<int> (std::max (2u, std::thread::hardware_concurrency()));
    cp.n_threads = c.threads > 0 ? c.threads : std::max (1, hw / 2);
    cp.n_threads_batch = cp.n_threads;
    cp.no_perf = true;
    impl->ctx = llama_init_from_model (impl->model, cp);
    if (impl->ctx == nullptr)
    {
        llama_model_free (impl->model);
        impl->model = nullptr;
        error = "모델 컨텍스트를 만들지 못했어요 (메모리 부족?)";
        return false;
    }
    cfg = c;
    impl->cached.clear();

    // Bring back the prompt cache kept in memopro while the model was unloaded.
    if (runtime != nullptr && impl->savedState != mem::kNoBuffer)
    {
        auto pin = runtime->pin (impl->savedState);
        if (pin && llama_state_seq_set_data (impl->ctx, pin.as<std::uint8_t>(), pin.size(), 0) > 0)
            impl->cached = impl->savedTokens;
    }
    return true;
#else
    (void) c;
    error = "이 빌드에는 로컬 언어 모델(llama.cpp)이 포함되어 있지 않아요";
    return false;
#endif
}

void LocalLlm::unload()
{
#if SMIX_HAVE_LLAMA
    std::lock_guard<std::mutex> g (lock);
    if (impl->ctx != nullptr)
    {
        // Keep the KV cache of the prompt prefix in the memory runtime (not on disk).
        if (runtime != nullptr && ! impl->cached.empty())
        {
            const auto size = llama_state_seq_get_size (impl->ctx, 0);
            std::vector<std::uint8_t> state (size);
            if (size > 0 && llama_state_seq_get_data (impl->ctx, state.data(), state.size(), 0) == size)
            {
                runtime->free (impl->savedState);
                impl->savedState = runtime->store (state.data(), state.size());
                impl->savedTokens = impl->cached;
            }
        }
        llama_free (impl->ctx);
        impl->ctx = nullptr;
    }
    if (impl->model != nullptr)
    {
        llama_model_free (impl->model);
        impl->model = nullptr;
    }
    impl->cached.clear();
#endif
}

LocalLlm::Generation LocalLlm::generate (const std::string& system, const std::string& user, const std::string& grammar,
                                         const std::function<bool (int)>& onToken)
{
    Generation out;
#if SMIX_HAVE_LLAMA
    std::lock_guard<std::mutex> g (lock);
    if (impl->ctx == nullptr)
    {
        out.error = "model not loaded";
        return out;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto* vocab = llama_model_get_vocab (impl->model);

    // Format with the model's own chat template (ChatML fallback).
    llama_chat_message msgs[2] = { { "system", system.c_str() }, { "user", user.c_str() } };
    const char* tmpl = llama_model_chat_template (impl->model, nullptr);
    std::vector<char> buf (system.size() + user.size() + 512);
    int len = llama_chat_apply_template (tmpl, msgs, 2, true, buf.data(), static_cast<int32_t> (buf.size()));
    if (len > static_cast<int> (buf.size()))
    {
        buf.resize (static_cast<size_t> (len));
        len = llama_chat_apply_template (tmpl, msgs, 2, true, buf.data(), static_cast<int32_t> (buf.size()));
    }
    std::string prompt = len > 0 ? std::string (buf.data(), static_cast<size_t> (len))
                                 : "<|im_start|>system\n" + system + "<|im_end|>\n<|im_start|>user\n" + user
                                       + "<|im_end|>\n<|im_start|>assistant\n";

    const int n = -llama_tokenize (vocab, prompt.c_str(), static_cast<int32_t> (prompt.size()), nullptr, 0, true, true);
    std::vector<llama_token> tokens (static_cast<size_t> (std::max (0, n)));
    if (n <= 0 || llama_tokenize (vocab, prompt.c_str(), static_cast<int32_t> (prompt.size()), tokens.data(),
                                  static_cast<int32_t> (tokens.size()), true, true) < 0)
    {
        out.error = "tokenize failed";
        return out;
    }
    const int nCtx = static_cast<int> (llama_n_ctx (impl->ctx));
    if (static_cast<int> (tokens.size()) + cfg.maxNewTokens > nCtx)
    {
        out.error = "prompt too long for the context";
        return out;
    }
    out.promptTokens = static_cast<int> (tokens.size());

    // Reuse the KV of the common prefix (the system prompt is identical between requests).
    size_t common = 0;
    while (common < impl->cached.size() && common < tokens.size() && impl->cached[common] == tokens[common])
        ++common;
    if (common == tokens.size())
        --common;  // at least one token must be decoded to get logits
    auto* memory = llama_get_memory (impl->ctx);
    llama_memory_seq_rm (memory, 0, static_cast<llama_pos> (common), -1);
    impl->cached.resize (common);
    out.reusedTokens = static_cast<int> (common);

    auto* sampler = llama_sampler_chain_init (llama_sampler_chain_default_params());
    if (! grammar.empty())
    {
        auto* gs = llama_sampler_init_grammar (vocab, grammar.c_str(), "root");
        if (gs == nullptr)
        {
            llama_sampler_free (sampler);
            out.error = "invalid grammar";
            return out;
        }
        llama_sampler_chain_add (sampler, gs);
    }
    if (cfg.temperature > 0.0f)
    {
        llama_sampler_chain_add (sampler, llama_sampler_init_temp (cfg.temperature));
        llama_sampler_chain_add (sampler, llama_sampler_init_dist (1234));
    }
    else
    {
        llama_sampler_chain_add (sampler, llama_sampler_init_greedy());
    }

    std::vector<llama_token> pending (tokens.begin() + static_cast<long> (common), tokens.end());
    const auto promptStart = std::chrono::steady_clock::now();
    bool first = true;
    for (int i = 0; i < cfg.maxNewTokens; ++i)
    {
        auto batch = llama_batch_get_one (pending.data(), static_cast<int32_t> (pending.size()));
        if (llama_decode (impl->ctx, batch) != 0)
        {
            out.error = "decode failed";
            break;
        }
        impl->cached.insert (impl->cached.end(), pending.begin(), pending.end());
        if (first)
        {
            const double s = std::chrono::duration<double> (std::chrono::steady_clock::now() - promptStart).count();
            if (pending.size() > 8 && s > 0)
                promptTps = 0.5 * promptTps + 0.5 * static_cast<double> (pending.size()) / s;
            first = false;
        }

        const llama_token t = llama_sampler_sample (sampler, impl->ctx, -1);
        if (llama_vocab_is_eog (vocab, t))
        {
            out.ok = true;
            break;
        }
        char piece[256];
        const int m = llama_token_to_piece (vocab, t, piece, sizeof (piece), 0, true);
        if (m > 0)
            out.text.append (piece, static_cast<size_t> (m));
        ++out.newTokens;
        pending.assign (1, t);
        if (onToken && ! onToken (out.newTokens))
            break;

        // A grammar-constrained JSON object is complete when the braces balance.
        if (! grammar.empty() && ! out.text.empty() && out.text.back() == '}')
        {
            const auto j = nlohmann::json::parse (out.text, nullptr, false);
            if (! j.is_discarded())
            {
                out.ok = true;
                break;
            }
        }
    }
    if (! out.ok && out.error.empty() && out.newTokens >= cfg.maxNewTokens)
        out.error = "answer too long";
    llama_sampler_free (sampler);

    out.seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    const double genSeconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - promptStart).count();
    if (out.newTokens > 4 && genSeconds > 0)
        tps = 0.5 * tps + 0.5 * out.newTokens / genSeconds;
#else
    (void) system; (void) user; (void) grammar; (void) onToken;
    out.error = "llama.cpp not compiled in";
#endif
    return out;
}

//==============================================================================
IntentResult LlmIntentModel::interpret (const IntentRequest& r)
{
    const auto gen = model.generate (intentSystemPrompt(), intentUserPrompt (r), intentGrammar());
    last = gen.text;
    if (! gen.ok)
        return {};
    const auto j = nlohmann::json::parse (gen.text, nullptr, false);
    if (j.is_discarded())
        return {};
    return parseModelJson (j, r);
}

} // namespace smix::llm
