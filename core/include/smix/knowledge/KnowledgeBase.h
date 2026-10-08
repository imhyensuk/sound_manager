#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "smix/knowledge/PluginProfile.h"
#include "smix/mem/MemoryRuntime.h"

namespace smix::knowledge
{

struct SearchHit
{
    std::string uid;
    std::string name;
    PluginCategory category = PluginCategory::Unknown;
    double score = 0.0;
    std::string snippet;
};

/**
    Long-term memory about the user's plugins (requirement 4: learn every plugin, remember it,
    retrieve it). Profiles are serialised into the memopro runtime (compressed when memory is
    short); only a small inverted index stays in ordinary memory. Retrieval is BM25 over
    Korean syllable bigrams and English words, with a mixing synonym table, so it works fully
    offline without an embedding model.
*/
class KnowledgeBase
{
public:
    explicit KnowledgeBase (mem::Runtime* runtime = nullptr);
    ~KnowledgeBase();

    void upsert (const PluginProfile&);
    void remove (const std::string& uid);
    std::optional<PluginProfile> get (const std::string& uid) const;
    bool contains (const std::string& uid) const;
    bool isCurrent (const std::string& uid) const;  // profiled with the current profiler version
    std::size_t size() const;
    std::vector<std::string> uids() const;

    std::vector<SearchHit> search (const std::string& query, std::size_t k = 5) const;

    /** Retrieved knowledge formatted for the language model's context (bounded size). */
    std::string contextFor (const std::string& query, std::size_t k = 4, std::size_t maxChars = 2400) const;

    /** JSON lines file, one profile per line. */
    bool save (const std::string& path) const;
    bool load (const std::string& path);

    static std::vector<std::string> tokenize (const std::string& text);

private:
    struct Entry
    {
        std::string name;
        PluginCategory category = PluginCategory::Unknown;
        int version = 0;
        mem::BufferId buffer = mem::kNoBuffer;
        std::string inlineJson;  // when no runtime
        std::map<std::string, int> termFreq;
        int length = 0;
    };

    void index (const std::string& uid, const PluginProfile&);
    void unindex (const std::string& uid);

    mem::Runtime* runtime;
    mutable std::mutex lock;
    std::map<std::string, Entry> entries;
    std::map<std::string, int> docFreq;
    long totalLength = 0;
};

} // namespace smix::knowledge
