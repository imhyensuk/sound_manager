#include "smix/MixHistory.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace smix
{
namespace
{
const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string toBase64 (const std::vector<std::uint8_t>& in)
{
    std::string out;
    out.reserve ((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3)
    {
        const std::uint32_t n = (static_cast<std::uint32_t> (in[i]) << 16)
                                | (i + 1 < in.size() ? static_cast<std::uint32_t> (in[i + 1]) << 8 : 0u)
                                | (i + 2 < in.size() ? static_cast<std::uint32_t> (in[i + 2]) : 0u);
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(n >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kB64[n & 63] : '=';
    }
    return out;
}

std::vector<std::uint8_t> fromBase64 (const std::string& in)
{
    std::vector<std::uint8_t> out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : in)
    {
        const char* p = std::strchr (kB64, c);
        if (c == '=' || p == nullptr || c == '\0')
            continue;
        buffer = (buffer << 6) | static_cast<std::uint32_t> (p - kB64);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back (static_cast<std::uint8_t> ((buffer >> bits) & 0xFF));
        }
    }
    return out;
}
} // namespace

MixHistory::MixHistory (mem::Runtime* r, std::size_t max) : runtime (r), maxSnapshots (max) {}

MixHistory::~MixHistory()
{
    clear();
}

void MixHistory::clear()
{
    if (runtime != nullptr)
        for (auto& [h, b] : blobs)
            runtime->free (b.buffer);
    blobs.clear();
    list.clear();
}

std::uint64_t MixHistory::hashBytes (const std::vector<std::uint8_t>& data)
{
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (auto b : data)
    {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h ^ data.size();
}

std::uint64_t MixHistory::keepBlob (const std::vector<std::uint8_t>& data)
{
    if (data.empty())
        return 0;
    const auto h = hashBytes (data);
    auto& b = blobs[h];
    if (b.refs++ == 0)
    {
        b.size = data.size();
        if (runtime != nullptr)
            b.buffer = runtime->store (data.data(), data.size());
        if (b.buffer == mem::kNoBuffer)
            b.inline_ = data;
    }
    return h;
}

std::vector<std::uint8_t> MixHistory::blobData (std::uint64_t hash) const
{
    auto it = blobs.find (hash);
    if (it == blobs.end())
        return {};
    if (it->second.buffer != mem::kNoBuffer && runtime != nullptr)
        return runtime->read (it->second.buffer);
    return it->second.inline_;
}

std::uint64_t MixHistory::blobBytes() const
{
    std::uint64_t total = 0;
    for (auto& [h, b] : blobs)
        total += b.size;
    return total;
}

void MixHistory::dropSnapshot (const Snapshot& s)
{
    for (auto& c : s.channels)
        for (auto& slot : c.chain)
        {
            auto it = blobs.find (slot.stateHash);
            if (it != blobs.end() && --it->second.refs <= 0)
            {
                if (runtime != nullptr)
                    runtime->free (it->second.buffer);
                blobs.erase (it);
            }
        }
}

std::int64_t MixHistory::record (const MixSession& session, const std::vector<std::string>& channelIds, const std::string& label,
                                 const std::string& source, double time, const BlobProvider& provider)
{
    Snapshot s;
    // Ids are milliseconds since the epoch: unique across sessions and sortable.
    s.id = std::max<std::int64_t> (lastId + 1, static_cast<std::int64_t> (time * 1000.0));
    lastId = s.id;
    s.time = time;
    s.label = label;
    s.source = source;

    for (auto& id : channelIds)
    {
        const auto* c = session.find (id);
        if (c == nullptr)
            continue;
        ChannelSnapshot cs;
        cs.channelId = c->id;
        cs.channelName = c->name;
        cs.gainDb = c->aiGainDb;
        for (size_t i = 0; i < c->chain.size(); ++i)
        {
            const auto& slot = c->chain[i];
            SlotSnapshot ss;
            ss.pluginUid = slot.pluginUid;
            ss.pluginName = slot.pluginName;
            ss.bypassed = slot.bypassed;
            for (auto& p : slot.params)
                ss.params.emplace_back (p.index, p.value);
            if (provider)
                ss.stateHash = keepBlob (provider (c->id, static_cast<int> (i)));
            cs.chain.push_back (std::move (ss));
        }
        s.channels.push_back (std::move (cs));
    }

    list.push_back (std::move (s));
    while (list.size() > maxSnapshots)
    {
        dropSnapshot (list.front());
        list.erase (list.begin());
    }
    return lastId;
}

const Snapshot* MixHistory::find (std::int64_t id) const
{
    for (auto& s : list)
        if (s.id == id)
            return &s;
    return nullptr;
}

RestorePlan MixHistory::planRestore (std::int64_t id, const MixSession& current) const
{
    RestorePlan plan;
    const auto* snap = find (id);
    if (snap == nullptr)
    {
        plan.notes.push_back ("기록을 찾을 수 없어요");
        return plan;
    }

    for (auto& cs : snap->channels)
    {
        const auto* c = current.find (cs.channelId);
        if (c == nullptr)
        {
            plan.notes.push_back ("'" + cs.channelName + "' 채널이 더 이상 없어요");
            continue;
        }
        if (c->protectedChannel)
        {
            plan.notes.push_back ("'" + cs.channelName + "' 채널은 보호되어 있어 되돌리지 않았어요");
            continue;
        }

        if (std::abs (c->aiGainDb - cs.gainDb) > 0.01f)
        {
            MixAction g;
            g.type = ActionType::SetGain;
            g.channelId = c->id;
            g.value = cs.gainDb;
            g.unit = "dB";
            g.reason = "restore";
            plan.actions.push_back (g);
        }

        bool sameChain = c->chain.size() == cs.chain.size();
        for (size_t i = 0; sameChain && i < cs.chain.size(); ++i)
            sameChain = c->chain[i].pluginUid == cs.chain[i].pluginUid;

        if (! sameChain)
        {
            RestorePlan::ChainReload r;
            r.channelId = c->id;
            for (auto& slot : cs.chain)
            {
                r.pluginUids.push_back (slot.pluginUid);
                r.states.push_back (blobData (slot.stateHash));
                r.bypassed.push_back (slot.bypassed);
            }
            plan.reloads.push_back (std::move (r));
            continue;
        }

        for (size_t i = 0; i < cs.chain.size(); ++i)
        {
            const auto& now = c->chain[i];
            const auto& then = cs.chain[i];
            if (now.protectedSlot)
            {
                plan.notes.push_back ("'" + cs.channelName + "'의 " + now.pluginName + "은(는) 보호되어 있어 되돌리지 않았어요");
                continue;
            }
            if (now.bypassed != then.bypassed)
            {
                MixAction b;
                b.type = ActionType::SetBypass;
                b.channelId = c->id;
                b.slot = static_cast<int> (i);
                b.bypass = then.bypassed;
                plan.actions.push_back (b);
            }
            for (auto& [index, value] : then.params)
            {
                const auto* p = now.findParam (index);
                if (p == nullptr || std::abs (p->value - value) < 1.0e-5f)
                    continue;
                MixAction a;
                a.type = ActionType::SetParam;
                a.channelId = c->id;
                a.slot = static_cast<int> (i);
                a.paramIndex = index;
                a.param = p->name;
                a.value = value;
                a.unit = "normalized";
                a.reason = "restore";
                plan.actions.push_back (a);
            }
        }
    }
    return plan;
}

nlohmann::json MixHistory::exportChannel (const std::string& channelId, std::size_t max) const
{
    nlohmann::json arr = nlohmann::json::array();
    const size_t first = list.size() > max ? list.size() - max : 0;
    for (size_t i = first; i < list.size(); ++i)
    {
        const auto& s = list[i];
        for (auto& c : s.channels)
        {
            if (c.channelId != channelId)
                continue;
            nlohmann::json chain = nlohmann::json::array();
            for (auto& slot : c.chain)
            {
                nlohmann::json params = nlohmann::json::array();
                for (auto& [idx, v] : slot.params)
                    params.push_back ({ idx, v });
                chain.push_back ({ { "uid", slot.pluginUid }, { "name", slot.pluginName }, { "bypassed", slot.bypassed },
                                   { "params", params }, { "state", toBase64 (blobData (slot.stateHash)) } });
            }
            arr.push_back ({ { "id", s.id }, { "time", s.time }, { "label", s.label }, { "source", s.source },
                             { "channel", c.channelId }, { "channel_name", c.channelName }, { "gain_db", c.gainDb },
                             { "chain", chain } });
        }
    }
    return arr;
}

void MixHistory::importChannel (const nlohmann::json& slices)
{
    if (! slices.is_array())
        return;
    for (auto& j : slices)
    {
        const auto id = j.value ("id", static_cast<std::int64_t> (0));
        auto it = std::find_if (list.begin(), list.end(), [id] (auto& s) { return s.id == id; });
        if (it == list.end())
        {
            Snapshot s;
            s.id = id;
            s.time = j.value ("time", 0.0);
            s.label = j.value ("label", std::string {});
            s.source = j.value ("source", std::string {});
            it = list.insert (std::upper_bound (list.begin(), list.end(), s, [] (auto& a, auto& b) { return a.id < b.id; }), s);
        }

        ChannelSnapshot c;
        c.channelId = j.value ("channel", std::string {});
        c.channelName = j.value ("channel_name", std::string {});
        c.gainDb = j.value ("gain_db", 0.0f);
        for (auto& slotJson : j.value ("chain", nlohmann::json::array()))
        {
            SlotSnapshot slot;
            slot.pluginUid = slotJson.value ("uid", std::string {});
            slot.pluginName = slotJson.value ("name", std::string {});
            slot.bypassed = slotJson.value ("bypassed", false);
            for (auto& p : slotJson.value ("params", nlohmann::json::array()))
                if (p.is_array() && p.size() == 2)
                    slot.params.emplace_back (p[0].get<int>(), p[1].get<float>());
            slot.stateHash = keepBlob (fromBase64 (slotJson.value ("state", std::string {})));
            c.chain.push_back (std::move (slot));
        }
        // Replace an existing slice of the same channel (re-import), otherwise add it.
        auto& channels = it->channels;
        for (auto& old : channels)
            if (old.channelId == c.channelId)
                for (auto& slot : old.chain)
                    if (auto b = blobs.find (slot.stateHash); b != blobs.end() && --b->second.refs <= 0)
                    {
                        if (runtime != nullptr)
                            runtime->free (b->second.buffer);
                        blobs.erase (b);
                    }
        channels.erase (std::remove_if (channels.begin(), channels.end(), [&] (auto& x) { return x.channelId == c.channelId; }),
                        channels.end());
        channels.push_back (std::move (c));
        lastId = std::max (lastId, id);
    }
    while (list.size() > maxSnapshots)
    {
        dropSnapshot (list.front());
        list.erase (list.begin());
    }
}

} // namespace smix
