// Evaluates a GGUF reasoning model on a JSONL test set (training/intent/make_dataset.py --split test)
// exactly as the plugin runs it: same system prompt, same grammar, greedy decoding.
//   smix_intent_eval <model.gguf> <test.jsonl> [max examples]
// Prints exact-match rates for targets / goals / view / command and the speed (tokens/s).

#include <chrono>
#include <fstream>
#include <iostream>
#include <set>

#include <smix/llm/LocalLlm.h>

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: smix_intent_eval model.gguf test.jsonl [max]\n";
        return 2;
    }
    const int maxExamples = argc > 3 ? std::stoi (argv[3]) : 200;

    smix::mem::Runtime runtime (256u << 20);
    smix::llm::LocalLlm llm (&runtime);
    smix::llm::LocalLlm::Config cfg;
    cfg.modelPath = argv[1];
    std::string error;
    if (! llm.load (cfg, error))
    {
        std::cerr << error << "\n";
        return 1;
    }

    std::ifstream in (argv[2]);
    std::string line;
    int n = 0, valid = 0, targetsOk = 0, goalsOk = 0, viewOk = 0, commandOk = 0, questionOk = 0;
    double seconds = 0;
    int tokens = 0;
    while (std::getline (in, line) && n < maxExamples)
    {
        const auto ex = nlohmann::json::parse (line, nullptr, false);
        if (ex.is_discarded() || ! ex.contains ("messages"))
            continue;
        const auto& msgs = ex["messages"];
        const std::string user = msgs[1]["content"];
        const auto expected = nlohmann::json::parse (msgs[2]["content"].get<std::string>());

        const auto gen = llm.generate (smix::llm::intentSystemPrompt(), user, smix::llm::intentGrammar());
        ++n;
        seconds += gen.seconds;
        tokens += gen.newTokens;
        const auto got = nlohmann::json::parse (gen.text, nullptr, false);
        if (! gen.ok || got.is_discarded())
            continue;
        ++valid;

        auto goalSet = [] (const nlohmann::json& j) {
            std::set<std::string> s;
            for (auto& g : j.value ("goals", nlohmann::json::array()))
                s.insert (g.value ("goal", std::string {}) + "@" + std::to_string (g.value ("amount", 1.0)));
            return s;
        };
        targetsOk += got.value ("targets", nlohmann::json::array()) == expected.value ("targets", nlohmann::json::array());
        goalsOk += goalSet (got) == goalSet (expected);
        viewOk += got.value ("view", std::string {}) == expected.value ("view", std::string {});
        commandOk += got.value ("command", std::string {}) == expected.value ("command", std::string {});
        questionOk += got.value ("question", std::string {}).empty() == expected.value ("question", std::string {}).empty();
    }

    auto pct = [n] (int x) { return n > 0 ? 100.0 * x / n : 0.0; };
    std::cout << "examples " << n << "\nvalid json " << pct (valid) << " %\ntargets " << pct (targetsOk) << " %\ngoals "
              << pct (goalsOk) << " %\nview " << pct (viewOk) << " %\ncommand " << pct (commandOk) << " %\nasks when it should "
              << pct (questionOk) << " %\nspeed " << (seconds > 0 ? tokens / seconds : 0) << " tokens/s, " << (n > 0 ? seconds / n : 0)
              << " s per request\n";
    return 0;
}
