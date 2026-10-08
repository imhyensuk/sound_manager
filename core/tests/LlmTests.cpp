#include <doctest/doctest.h>

#include <fstream>

#include <smix/llm/LocalLlm.h>

#include "TestHelpers.h"

using namespace smix;
using namespace smix::llm;

TEST_CASE ("model JSON is turned into targets and goals; grammar text is well formed")
{
    const auto session = test::makeSession();
    IntentRequest r;
    r.session = &session;
    r.scopeRootId = "m";
    const auto j = nlohmann::json::parse (R"({"reply":"ok","targets":["kick"],"goals":[{"goal":"tighter","amount":0.5}],
        "question":"","options":[],"view":"","command":""})");
    const auto res = parseModelJson (j, r);
    CHECK (res.understood);
    CHECK (res.targets == std::vector<std::string> { "k" });
    REQUIRE (res.goals.size() == 1);
    CHECK ((res.goals[0].goal == SoundGoal::Tighter));
    CHECK (intentGrammar().find ("root ::=") == 0);
    CHECK (intentUserPrompt (r).find ("Kick In") != std::string::npos);
}

TEST_CASE ("the training data uses exactly the plugin's prompt and grammar")
{
    auto slurp = [] (const std::string& path) {
        std::ifstream f (path);
        return std::string ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    };
    CHECK (slurp (SMIX_SOURCE_DIR "/training/intent/system_prompt.txt") == intentSystemPrompt());
    CHECK (slurp (SMIX_SOURCE_DIR "/training/intent/grammar.gbnf") == intentGrammar());
}

TEST_CASE ("local LLM (llama.cpp): load, grammar-constrained JSON, prompt cache, memopro state across unload")
{
    if (! LocalLlm::compiledIn())
    {
        MESSAGE ("llama.cpp not compiled in - skipped");
        return;
    }
    if (! std::ifstream (SMIX_TEST_TINY_LLM))
    {
        MESSAGE ("tiny test model missing (" << SMIX_TEST_TINY_LLM << ") - run training/tools/make_tiny_gguf.py");
        return;
    }

    mem::Runtime rt (64u << 20);
    LocalLlm llm (&rt);
    LocalLlm::Config cfg;
    cfg.modelPath = SMIX_TEST_TINY_LLM;
    cfg.contextTokens = 1024;
    cfg.maxNewTokens = 64;
    std::string err;
    REQUIRE (llm.load (cfg, err));

    const std::string grammar = R"(root ::= "{\"ok\":" ("true" | "false") "}")";
    auto g = llm.generate ("system prompt that stays the same", "first question", grammar);
    INFO (g.error);
    REQUIRE (g.ok);
    const auto j = nlohmann::json::parse (g.text, nullptr, false);
    REQUIRE_FALSE (j.is_discarded());
    CHECK (j.contains ("ok"));

    // Same system prompt: its KV is reused.
    g = llm.generate ("system prompt that stays the same", "second question", grammar);
    CHECK (g.ok);
    CHECK (g.reusedTokens > 5);

    // The intent grammar compiles in llama.cpp (the random model may ramble; that is fine).
    cfg.maxNewTokens = 8;
    g = llm.generate ("s", "u", intentGrammar());
    CHECK (g.error != "invalid grammar");

    // Unload keeps the prompt cache in memopro; after reloading it is used again.
    llm.unload();
    CHECK_FALSE (llm.isLoaded());
    CHECK (rt.stats().buffers >= 1);
    REQUIRE (llm.load (cfg, err));
    g = llm.generate ("s", "u", grammar);
    CHECK (g.ok);
    CHECK (g.reusedTokens > 0);

    LlmIntentModel model (llm);
    const auto session = test::makeSession();
    IntentRequest r;
    r.userText = "킥을 단단하게";
    r.session = &session;
    r.scopeRootId = "m";
    model.interpret (r);  // must not crash on nonsense output
}
