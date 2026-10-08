#include "smix/knowledge/KnowledgeBase.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>

#include "smix/Types.h"

namespace smix::knowledge
{
namespace
{
/** Decodes one UTF-8 code point; returns its length in bytes. */
int decodeUtf8 (const std::string& s, size_t i, std::uint32_t& cp)
{
    const auto c = static_cast<unsigned char> (s[i]);
    if (c < 0x80) { cp = c; return 1; }
    if ((c >> 5) == 0x6 && i + 1 < s.size()) { cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char> (s[i + 1]) & 0x3Fu); return 2; }
    if ((c >> 4) == 0xE && i + 2 < s.size())
    {
        cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char> (s[i + 1]) & 0x3Fu) << 6) | (static_cast<unsigned char> (s[i + 2]) & 0x3Fu);
        return 3;
    }
    if ((c >> 3) == 0x1E && i + 3 < s.size()) { cp = 0; return 4; }
    cp = 0;
    return 1;
}

bool isHangul (std::uint32_t cp) { return cp >= 0xAC00 && cp <= 0xD7A3; }

/** Mixing vocabulary bridging Korean requests and the English parameter/category words. */
const std::vector<std::pair<const char*, const char*>>& synonyms()
{
    static const std::vector<std::pair<const char*, const char*>> s {
        { "컴프", "compressor compression" }, { "압축", "compressor compression" }, { "리미터", "limiter ceiling" },
        { "이큐", "eq" }, { "eq", "eq band gain freq" }, { "리버브", "reverb decay" }, { "잔향", "reverb decay tail" },
        { "딜레이", "delay feedback" }, { "에코", "delay echo" }, { "디에서", "deesser sibilance" }, { "치찰", "deesser sibilance" },
        { "새츄", "saturation drive harmonics" }, { "배음", "saturation harmonics" }, { "왜곡", "distortion drive" },
        { "단단", "attack transient compression tight" }, { "펀치", "transient attack punch" }, { "어택", "attack transient" },
        { "넓", "width wider stereo" }, { "스테레오", "stereo width" }, { "밝", "presence air bright" }, { "따뜻", "warm saturation mud" },
        { "탁", "mud low-mid" }, { "공기", "air" }, { "저역", "bass sub" }, { "게이트", "gate" }, { "볼륨", "level gain output" },
    };
    return s;
}
} // namespace

std::vector<std::string> KnowledgeBase::tokenize (const std::string& textIn)
{
    std::vector<std::string> tokens;
    std::string word;
    std::vector<std::string> hangulRun;

    auto flushWord = [&] {
        if (word.size() >= 2)
            tokens.push_back (word);
        word.clear();
    };
    auto flushHangul = [&] {
        if (hangulRun.size() == 1)
            tokens.push_back (hangulRun[0]);
        for (size_t i = 1; i < hangulRun.size(); ++i)
            tokens.push_back (hangulRun[i - 1] + hangulRun[i]);  // syllable bigrams handle Korean inflection
        hangulRun.clear();
    };

    const auto text = toLowerAscii (textIn);
    for (size_t i = 0; i < text.size();)
    {
        std::uint32_t cp = 0;
        const int len = decodeUtf8 (text, i, cp);
        if (isHangul (cp))
        {
            flushWord();
            hangulRun.push_back (text.substr (i, static_cast<size_t> (len)));
        }
        else if (cp < 128 && (std::isalnum (static_cast<int> (cp)) != 0))
        {
            flushHangul();
            word.push_back (static_cast<char> (cp));
        }
        else
        {
            flushWord();
            flushHangul();
        }
        i += static_cast<size_t> (len);
    }
    flushWord();
    flushHangul();
    return tokens;
}

KnowledgeBase::KnowledgeBase (mem::Runtime* r) : runtime (r) {}

KnowledgeBase::~KnowledgeBase()
{
    if (runtime != nullptr)
        for (auto& [uid, e] : entries)
            runtime->free (e.buffer);
}

void KnowledgeBase::unindex (const std::string& uid)
{
    auto it = entries.find (uid);
    if (it == entries.end())
        return;
    for (auto& [term, tf] : it->second.termFreq)
        if (--docFreq[term] <= 0)
            docFreq.erase (term);
    totalLength -= it->second.length;
    if (runtime != nullptr)
        runtime->free (it->second.buffer);
    entries.erase (it);
}

void KnowledgeBase::index (const std::string& uid, const PluginProfile& p)
{
    Entry e;
    e.name = p.name;
    e.category = p.measuredCategory != PluginCategory::Unknown ? p.measuredCategory : p.declaredCategory;
    e.version = p.version;
    const auto json = p.toJson().dump();
    if (runtime != nullptr)
        e.buffer = runtime->store (json);
    if (e.buffer == mem::kNoBuffer)
        e.inlineJson = json;

    for (auto& t : tokenize (p.document() + " " + toString (p.declaredCategory) + " " + toString (p.measuredCategory)))
        ++e.termFreq[t];
    for (auto& [term, tf] : e.termFreq)
    {
        ++docFreq[term];
        e.length += tf;
    }
    totalLength += e.length;
    entries[uid] = std::move (e);
}

void KnowledgeBase::upsert (const PluginProfile& p)
{
    std::lock_guard<std::mutex> g (lock);
    unindex (p.uid);
    index (p.uid, p);
}

void KnowledgeBase::remove (const std::string& uid)
{
    std::lock_guard<std::mutex> g (lock);
    unindex (uid);
}

std::optional<PluginProfile> KnowledgeBase::get (const std::string& uid) const
{
    std::string json;
    {
        std::lock_guard<std::mutex> g (lock);
        auto it = entries.find (uid);
        if (it == entries.end())
            return std::nullopt;
        json = it->second.buffer != mem::kNoBuffer && runtime != nullptr ? runtime->readText (it->second.buffer)
                                                                         : it->second.inlineJson;
    }
    const auto j = nlohmann::json::parse (json, nullptr, false);
    if (j.is_discarded())
        return std::nullopt;
    return PluginProfile::fromJson (j);
}

bool KnowledgeBase::contains (const std::string& uid) const
{
    std::lock_guard<std::mutex> g (lock);
    return entries.count (uid) != 0;
}

bool KnowledgeBase::isCurrent (const std::string& uid) const
{
    std::lock_guard<std::mutex> g (lock);
    auto it = entries.find (uid);
    return it != entries.end() && it->second.version >= PluginProfile::kVersion;
}

std::size_t KnowledgeBase::size() const
{
    std::lock_guard<std::mutex> g (lock);
    return entries.size();
}

std::vector<std::string> KnowledgeBase::uids() const
{
    std::lock_guard<std::mutex> g (lock);
    std::vector<std::string> out;
    for (auto& [uid, e] : entries)
        out.push_back (uid);
    return out;
}

std::vector<SearchHit> KnowledgeBase::search (const std::string& query, std::size_t k) const
{
    std::string expanded = query;
    const auto lower = toLowerAscii (query);
    for (auto& [word, extra] : synonyms())
        if (lower.find (word) != std::string::npos)
            expanded += std::string (" ") + extra;

    std::set<std::string> terms;
    for (auto& t : tokenize (expanded))
        terms.insert (t);

    std::lock_guard<std::mutex> g (lock);
    const double n = static_cast<double> (entries.size());
    const double avgLength = entries.empty() ? 1.0 : static_cast<double> (totalLength) / n;
    constexpr double k1 = 1.2, b = 0.75;

    std::vector<SearchHit> hits;
    for (auto& [uid, e] : entries)
    {
        double score = 0;
        for (auto& t : terms)
        {
            auto tf = e.termFreq.find (t);
            if (tf == e.termFreq.end())
                continue;
            const double df = docFreq.at (t);
            const double idf = std::log (1.0 + (n - df + 0.5) / (df + 0.5));
            const double f = tf->second;
            score += idf * f * (k1 + 1) / (f + k1 * (1 - b + b * e.length / avgLength));
        }
        if (score > 0)
            hits.push_back ({ uid, e.name, e.category, score, {} });
    }
    std::sort (hits.begin(), hits.end(), [] (auto& a, auto& c) { return a.score > c.score; });
    if (hits.size() > k)
        hits.resize (k);
    return hits;
}

std::string KnowledgeBase::contextFor (const std::string& query, std::size_t k, std::size_t maxChars) const
{
    std::string out;
    for (auto& hit : search (query, k))
    {
        auto p = get (hit.uid);
        if (! p)
            continue;
        auto doc = p->document();
        const size_t room = maxChars > out.size() ? maxChars - out.size() : 0;
        if (room < 80)
            break;
        if (doc.size() > room)
            doc = doc.substr (0, room) + "…";
        out += doc + "\n";
    }
    return out;
}

bool KnowledgeBase::save (const std::string& path) const
{
    std::ofstream f (path, std::ios::trunc);
    if (! f)
        return false;
    for (auto& uid : uids())
        if (auto p = get (uid))
            f << p->toJson().dump() << "\n";
    return static_cast<bool> (f);
}

bool KnowledgeBase::load (const std::string& path)
{
    std::ifstream f (path);
    if (! f)
        return false;
    std::string line;
    while (std::getline (f, line))
    {
        const auto j = nlohmann::json::parse (line, nullptr, false);
        if (! j.is_discarded())
            upsert (PluginProfile::fromJson (j));
    }
    return true;
}

} // namespace smix::knowledge
