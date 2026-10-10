#pragma once

#include <juce_events/juce_events.h>
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <smix/MixSession.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>

/** Lifetime token: async callbacks check it before touching the link. */
struct SessionLinkAlive
{
    JUCE_DECLARE_WEAK_REFERENCEABLE (SessionLinkAlive)
};

/**
    Links Sound Manager instances that live in different processes of this computer (a DAW that
    runs some plugins out of process, Logic Pro's AU hosting service, several hosts at once).

    Local only: a TCP socket on 127.0.0.1, never another machine. The first process to start
    listens on the port and relays; every other process connects to it. When that process goes
    away, another one takes over within a few seconds.

    Every process publishes its own channels (features, chain, parameters and their value curves)
    once a second; every process sees the union, so a master in one process mixes tracks in
    another. Changes to a remote channel travel as validated low-level calls (set a parameter,
    gain, bypass, chain) to the process that owns it.

    Message thread only.
*/
class SessionLink : private juce::Timer
{
public:
    /** fn: "set_param", "set_gain", "set_bypass", "set_chain", "move_slot". */
    using CallHandler = std::function<void (const nlohmann::json& call)>;

    explicit SessionLink (CallHandler);
    ~SessionLink() override;

    /** Session key: only processes with the same key are linked (default "default"). */
    void start (const juce::String& sessionKey, int port);
    void stop();
    bool isRunning() const noexcept { return running; }

    /** This process's channels, published every second (call from the hub's refresh). */
    void publish (const std::vector<smix::ChannelState>& local, const std::map<std::string, bool>& autoMix);

    /** Channels owned by other processes (fresh ones only). */
    std::vector<smix::ChannelState> remoteChannels() const;
    bool isRemote (const std::string& channelId) const;
    /** Whether the remote instance runs its own auto mix (for "mixed by an ancestor"). */
    bool remoteAutoMix (const std::string& channelId) const;

    /** Sends a call to the process that owns the channel. */
    bool call (const std::string& channelId, nlohmann::json call);

    juce::String describe() const;
    const juce::String& processId() const noexcept { return procId; }

private:
    class Connection;
    class Server;
    friend class Connection;
    friend class Server;

    void timerCallback() override;
    void received (Connection*, const nlohmann::json&);
    void send (Connection*, const nlohmann::json&);
    void broadcast (const nlohmann::json&, Connection* except);
    void connectionLost (Connection*);
    void tryConnect();

    struct Peer
    {
        nlohmann::json channels = nlohmann::json::array();
        std::map<std::string, nlohmann::json> mappers;  // channel id + slot + signature -> mappers
        std::map<std::string, bool> autoMix;
        double lastSeen = 0.0;
        Connection* via = nullptr;  // server: the connection it came from
    };

    CallHandler onCall;
    std::unique_ptr<SessionLinkAlive> alive;
    juce::CriticalSection clientLock;  // clients are added on the server thread
    juce::String procId, key;
    int port = 0;
    bool running = false;
    std::unique_ptr<Server> server;          // this process relays
    std::unique_ptr<Connection> client;      // this process is connected to the relay
    std::vector<std::unique_ptr<Connection>> clients;  // server side
    std::map<juce::String, Peer> peers;
    nlohmann::json lastLocal = nlohmann::json::array();
    std::map<std::string, std::string> sentSignatures;  // channel/slot -> signature already sent with mappers
    std::map<std::string, bool> lastAutoMix;
    int ticks = 0;
};
