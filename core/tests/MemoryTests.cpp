#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <thread>

#include <smix/mem/MemoryRuntime.h>
#include <smix/modules/ModuleManager.h>

using namespace smix;

TEST_CASE ("memory runtime: store/read round trip, budget refusal, file buffers")
{
    mem::Runtime rt (4u << 20);  // 4 MiB
    INFO ("backend: " << (rt.stats().memopro ? "memopro " + mem::Runtime::memoproVersion() : std::string ("fallback")));

    std::vector<std::uint8_t> blob (300 * 1024);
    for (size_t i = 0; i < blob.size(); ++i)
        blob[i] = static_cast<std::uint8_t> ((i * 31) ^ (i >> 7));
    const auto id = rt.store (blob.data(), blob.size());
    REQUIRE (id != mem::kNoBuffer);
    CHECK (rt.read (id) == blob);
    CHECK (rt.readText (rt.store (std::string ("안녕 memopro"))) == "안녕 memopro");
    CHECK (rt.store (nullptr, 0) == mem::kNoBuffer);

    // Far more than the budget is refused, never swapped.
    CHECK (rt.allocate (64u << 20) == mem::kNoBuffer);
    CHECK (rt.stats().refusals >= 1);
    CHECK (rt.stats().writtenBytes == 0);

    // A file region larger than what is left still reads back exactly (dropped and re-read).
    const std::string path = "smix-mem-test.bin";
    {
        std::ofstream f (path, std::ios::binary);
        for (int i = 0; i < (3 << 20); ++i)
            f.put (static_cast<char> (i * 7));
    }
    std::vector<mem::BufferId> parts;
    for (int i = 0; i < 3; ++i)
        parts.push_back (rt.addFile (path, static_cast<std::uint64_t> (i) << 20, 1u << 20));
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < 3; ++i)
        {
            auto pin = rt.pin (parts[static_cast<size_t> (i)]);
            REQUIRE (pin);
            CHECK (pin.as<std::uint8_t>()[12345] == static_cast<std::uint8_t> (((i << 20) + 12345) * 7));
        }
    CHECK (rt.stats().peakUsed <= rt.budget());
    std::remove (path.c_str());
}

TEST_CASE ("module manager: on demand, idle unload, user switch, memory budget")
{
    using namespace smix::modules;
    int earLoads = 0, llmLoads = 0, llmUnloads = 0;
    ModuleManager mm (100);
    mm.add (std::make_unique<LambdaModule> (ModuleId::Ear, [&] (std::string&) { ++earLoads; return true; }, [] {}, 10),
            { 0.0, true, true });
    mm.add (std::make_unique<LambdaModule> (ModuleId::Intent, [&] (std::string&) { ++llmLoads; return true; }, [&] { ++llmUnloads; }, 80),
            { 60.0, true, false });
    mm.add (std::make_unique<LambdaModule> (ModuleId::Knowledge, [] (std::string&) { return true; }, [] {}, 50), { 60.0, true, false });
    mm.add (std::make_unique<LambdaModule> (ModuleId::Style, [] (std::string& e) { e = "broken"; return false; }, [] {}, 1));

    CHECK_FALSE (mm.isLoaded (ModuleId::Intent));  // nothing runs until needed
    {
        auto lease = mm.acquire (ModuleId::Intent, 0.0);
        CHECK (lease);
        CHECK ((mm.isLoaded (ModuleId::Intent)));
        CHECK (mm.tick (1000.0).empty());  // in use: never unloaded
    }
    CHECK (mm.tick (30.0).empty());
    CHECK ((mm.tick (61.0) == std::vector<ModuleId> { ModuleId::Intent }));
    CHECK (llmUnloads == 1);

    // Loading the knowledge base (50) while the LLM (80) is idle exceeds the budget of 100:
    // the idle LLM is unloaded first.
    { auto l = mm.acquire (ModuleId::Intent, 100.0); }
    { auto k = mm.acquire (ModuleId::Knowledge, 101.0); CHECK (k); }
    CHECK_FALSE ((mm.isLoaded (ModuleId::Intent)));
    CHECK (llmUnloads == 2);

    mm.setEnabled (ModuleId::Intent, false);
    CHECK_FALSE ((mm.acquire (ModuleId::Intent, 200.0)));
    CHECK (llmLoads == 2);

    std::string err;
    CHECK_FALSE ((mm.acquire (ModuleId::Style, 0.0, &err)));
    CHECK (err == "broken");
    CHECK ((mm.state (ModuleId::Style) == ModuleState::Failed));
    CHECK (mm.status (0.0)["modules"].size() == 4);
}
