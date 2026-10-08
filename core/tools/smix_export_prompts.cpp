// Writes the reasoning module's system prompt, output grammar and a user-prompt example, so the
// training data (training/intent) is built with exactly what the plugin sends to the model.
//   smix_export_prompts <out dir>

#include <fstream>
#include <iostream>

#include <smix/llm/IntentModel.h>

int main (int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    std::ofstream (dir + "/system_prompt.txt") << smix::llm::intentSystemPrompt();
    std::ofstream (dir + "/grammar.gbnf") << smix::llm::intentGrammar();
    std::cout << "wrote " << dir << "/system_prompt.txt and grammar.gbnf\n";
    return 0;
}
