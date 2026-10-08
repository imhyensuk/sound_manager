#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "smix/llm/IntentModel.h"
#include "smix/mem/MemoryRuntime.h"

namespace smix::llm
{

/**
    A local GGUF language model run by llama.cpp, fully offline (no network code is compiled).

    Memory: weights are memory-mapped from the GGUF file (clean, file-backed pages the OS can
    drop and re-read, like memopro's file buffers); the module manager loads the model only
    while chat is used. On unload the KV cache of the fixed system prompt is kept in the memopro
    runtime (compressed when memory is short), so the next load skips re-reading the prompt.
*/
class LocalLlm
{
public:
    struct Config
    {
        std::string modelPath;
        int contextTokens = 4096;
        int threads = 0;          // 0 = half the hardware threads
        int gpuLayers = 0;        // > 0 offloads layers (Metal/CUDA builds)
        int maxNewTokens = 384;
        float temperature = 0.0f; // 0 = greedy (deterministic)
    };

    struct Generation
    {
        bool ok = false;
        std::string text;
        std::string error;
        int promptTokens = 0, reusedTokens = 0, newTokens = 0;
        double seconds = 0.0;
    };

    explicit LocalLlm (mem::Runtime* runtime = nullptr);
    ~LocalLlm();

    static bool compiledIn() noexcept;

    bool load (const Config&, std::string& error);
    void unload();
    bool isLoaded() const;

    /** Chat-template formatted single turn; `grammar` (GBNF) constrains the output if not empty. */
    Generation generate (const std::string& system, const std::string& user, const std::string& grammar,
                         const std::function<bool (int newTokens)>& onToken = {});

    /** Measured decode speed (tokens/s) for ETA display; prior when nothing measured yet. */
    double tokensPerSecond() const noexcept { return tps; }
    double promptTokensPerSecond() const noexcept { return promptTps; }
    std::uint64_t memoryEstimate() const;
    const Config& config() const noexcept { return cfg; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    mem::Runtime* runtime;
    Config cfg;
    mutable std::mutex lock;
    double tps = 8.0, promptTps = 80.0;
};

/** The reasoning module: requests and context understood by the local language model. */
class LlmIntentModel : public IntentModel
{
public:
    explicit LlmIntentModel (LocalLlm& llm) : model (llm) {}
    std::string name() const override { return "local-llm"; }
    IntentResult interpret (const IntentRequest&) override;

    /** Last raw model output (for diagnostics). */
    const std::string& lastOutput() const noexcept { return last; }

private:
    LocalLlm& model;
    std::string last;
};

} // namespace smix::llm
