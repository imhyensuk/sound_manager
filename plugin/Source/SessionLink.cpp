#include "SessionLink.h"

#include <smix/SessionWire.h>

namespace
{
constexpr juce::uint32 kMagic = 0x534d4c4b;  // "SMLK"
constexpr double kStaleSeconds = 4.0;
constexpr int kProtocol = 1;

double nowSeconds() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }
} // namespace

//==============================================================================
class SessionLink::Connection : public juce::InterprocessConnection
{
public:
    explicit Connection (SessionLink& l) : juce::InterprocessConnection (true, kMagic), link (l) {}
    ~Connection() override { disconnect(); }

    void connectionMade() override {}
    void connectionLost() override
    {
        // Never delete a connection inside its own callback: let the link clean up afterwards.
        juce::MessageManager::callAsync ([safe = juce::WeakReference<SessionLinkAlive> (link.alive.get()), this] {
            if (safe != nullptr)
                link.connectionLost (this);
        });
    }
    void messageReceived (const juce::MemoryBlock& data) override
    {
        const auto j = nlohmann::json::parse (static_cast<const char*> (data.getData()),
                                              static_cast<const char*> (data.getData()) + data.getSize(), nullptr, false);
        if (! j.is_discarded() && j.is_object())
            link.received (this, j);
    }

    SessionLink& link;
};

class SessionLink::Server : public juce::InterprocessConnectionServer
{
public:
    explicit Server (SessionLink& l) : link (l) {}
    ~Server() override { stop(); }

    juce::InterprocessConnection* createConnectionObject() override
    {
        // Called on the server thread; the link owns every connection.
        auto* c = new Connection (link);
        const juce::ScopedLock sl (link.clientLock);
        link.clients.emplace_back (c);
        return c;
    }

    SessionLink& link;
};

//==============================================================================
SessionLink::SessionLink (CallHandler handler) : onCall (std::move (handler)), alive (std::make_unique<SessionLinkAlive>())
{
    procId = juce::String::toHexString (juce::Random().nextInt64());
}

SessionLink::~SessionLink()
{
    stop();
}

void SessionLink::start (const juce::String& sessionKey, int listenPort)
{
    stop();
    key = sessionKey.isEmpty() ? juce::String ("default") : sessionKey;
    port = listenPort;
    running = true;
    tryConnect();
    startTimer (1000);
}

void SessionLink::stop()
{
    stopTimer();
    running = false;
    if (server != nullptr)
        server->stop();
    server.reset();
    client.reset();
    {
        const juce::ScopedLock sl (clientLock);
        clients.clear();
    }
    peers.clear();
    sentSignatures.clear();
}

void SessionLink::tryConnect()
{
    if (! running || server != nullptr || (client != nullptr && client->isConnected()))
        return;
    // Relay already running somewhere? Join it. Otherwise become the relay.
    auto c = std::make_unique<Connection> (*this);
    if (c->connectToSocket ("127.0.0.1", port, 300))
    {
        client = std::move (c);
        sentSignatures.clear();  // the relay may be new: send every value curve again
        send (client.get(), { { "t", "hello" }, { "proc", procId.toStdString() }, { "key", key.toStdString() }, { "v", kProtocol } });
        return;
    }
    auto s = std::make_unique<Server> (*this);
    if (s->beginWaitingForSocket (port, "127.0.0.1"))
        server = std::move (s);
}

void SessionLink::send (Connection* c, const nlohmann::json& j)
{
    if (c == nullptr || ! c->isConnected())
        return;
    const auto text = j.dump();
    c->sendMessage (juce::MemoryBlock (text.data(), text.size()));
}

void SessionLink::broadcast (const nlohmann::json& j, Connection* except)
{
    if (client != nullptr)
        send (client.get(), j);
    const juce::ScopedLock sl (clientLock);
    for (auto& c : clients)
        if (c.get() != except)
            send (c.get(), j);
}

void SessionLink::connectionLost (Connection* c)
{
    if (c == client.get())
    {
        client.reset();
        peers.clear();  // the relay is gone: it (and everything it relayed) will reappear after reconnecting
        return;
    }
    std::unique_ptr<Connection> dead;
    {
        const juce::ScopedLock sl (clientLock);
        for (auto it = clients.begin(); it != clients.end(); ++it)
            if (it->get() == c)
            {
                dead = std::move (*it);
                clients.erase (it);
                break;
            }
    }
    for (auto it = peers.begin(); it != peers.end();)
        it = it->second.via == c ? peers.erase (it) : std::next (it);
}

void SessionLink::received (Connection* from, const nlohmann::json& j)
{
    if (j.value ("key", std::string {}) != key.toStdString())
        return;  // another session (another project or host): not ours
    const auto type = j.value ("t", std::string {});
    const juce::String sender (j.value ("proc", std::string {}));
    if (sender == procId)
        return;

    if (type == "hello")
    {
        sentSignatures.clear();  // a newcomer needs the value curves
        return;
    }

    if (type == "state")
    {
        auto& peer = peers[sender];
        peer.lastSeen = nowSeconds();
        peer.via = server != nullptr ? from : nullptr;
        peer.channels = j.value ("channels", nlohmann::json::array());
        peer.autoMix.clear();
        const auto am = j.value ("auto_mix", nlohmann::json::object());
        for (auto it = am.begin(); it != am.end(); ++it)
            peer.autoMix[it.key()] = it.value().get<bool>();
        // value curves arrive with the first state after a slot changes; keep them by signature
        for (auto& ch : peer.channels)
            if (ch.contains ("chain"))
                for (auto& slot : ch["chain"])
                    if (slot.contains ("mappers"))
                        peer.mappers[ch.value ("id", std::string {}) + "|" + slot.value ("sig", std::string {})] = slot["mappers"];
        if (server != nullptr)
            broadcast (j, from);  // relay to everyone else
        return;
    }

    if (type == "call")
    {
        const juce::String to (j.value ("to", std::string {}));
        if (to == procId)
        {
            if (onCall)
                onCall (j);
        }
        else if (server != nullptr)
        {
            // relay to the process that owns the channel
            if (auto it = peers.find (to); it != peers.end() && it->second.via != nullptr)
                send (it->second.via, j);
        }
    }
}

void SessionLink::publish (const std::vector<smix::ChannelState>& local, const std::map<std::string, bool>& autoMix)
{
    nlohmann::json channels = nlohmann::json::array();
    for (auto& c : local)
    {
        // Value curves only when a slot is new or changed (they are large and static).
        bool withMappers = false;
        for (auto& s : c.chain)
        {
            const auto k = c.id + "|" + smix::wire::slotSignature (s);
            if (sentSignatures.count (k) == 0)
            {
                withMappers = true;
                sentSignatures[k] = k;
            }
        }
        channels.push_back (smix::wire::channelToJson (c, withMappers));
    }
    lastLocal = channels;
    lastAutoMix = autoMix;
}

void SessionLink::timerCallback()
{
    if (! running)
        return;
    ++ticks;
    if (server == nullptr && (client == nullptr || ! client->isConnected()))
        tryConnect();  // the relay went away: reconnect or take over

    nlohmann::json am = nlohmann::json::object();
    for (auto& [id, on] : lastAutoMix) am[id] = on;
    broadcast ({ { "t", "state" }, { "proc", procId.toStdString() }, { "key", key.toStdString() }, { "channels", lastLocal },
                 { "auto_mix", am } }, nullptr);
    // Mappers were in this message; next ones are light.
    for (auto& ch : lastLocal)
        if (ch.contains ("chain"))
            for (auto& slot : ch["chain"])
                slot.erase ("mappers");

    const double now = nowSeconds();
    for (auto it = peers.begin(); it != peers.end();)
        it = now - it->second.lastSeen > kStaleSeconds ? peers.erase (it) : std::next (it);
}

std::vector<smix::ChannelState> SessionLink::remoteChannels() const
{
    std::vector<smix::ChannelState> out;
    for (auto& [proc, peer] : peers)
        for (auto& ch : peer.channels)
        {
            auto c = smix::wire::channelFromJson (ch);
            // attach cached value curves to slots that came without them
            for (size_t s = 0; s < c.chain.size() && s < ch["chain"].size(); ++s)
            {
                if (! c.chain[s].mappers.empty())
                    continue;
                const auto k = c.id + "|" + ch["chain"][s].value ("sig", std::string {});
                if (auto m = peer.mappers.find (k); m != peer.mappers.end())
                    for (auto it = m->second.begin(); it != m->second.end(); ++it)
                        c.chain[s].mappers[std::stoi (it.key())] = smix::ValueMapper::fromJson (it.value());
            }
            out.push_back (std::move (c));
        }
    return out;
}

bool SessionLink::isRemote (const std::string& id) const
{
    for (auto& [proc, peer] : peers)
        for (auto& ch : peer.channels)
            if (ch.value ("id", std::string {}) == id)
                return true;
    return false;
}

bool SessionLink::remoteAutoMix (const std::string& id) const
{
    for (auto& [proc, peer] : peers)
        if (auto it = peer.autoMix.find (id); it != peer.autoMix.end())
            return it->second;
    return false;
}

bool SessionLink::call (const std::string& channelId, nlohmann::json c)
{
    for (auto& [proc, peer] : peers)
        for (auto& ch : peer.channels)
            if (ch.value ("id", std::string {}) == channelId)
            {
                c["t"] = "call";
                c["key"] = key.toStdString();
                c["proc"] = procId.toStdString();
                c["to"] = proc.toStdString();
                c["channel"] = channelId;
                if (server != nullptr)
                    send (peer.via, c);
                else
                    send (client.get(), c);
                return true;
            }
    return false;
}

juce::String SessionLink::describe() const
{
    if (! running)
        return "off";
    int remote = 0;
    for (auto& [p, peer] : peers)
        remote += static_cast<int> (peer.channels.size());
    juce::String role = server != nullptr ? "relay" : (client != nullptr && client->isConnected() ? "joined" : "waiting");
    return role + ", " + juce::String (static_cast<int> (peers.size())) + " other process(es), " + juce::String (remote) + " remote channel(s)";
}
