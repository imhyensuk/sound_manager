#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "smix/MixSession.h"

namespace smix::wire
{

/**
    Complete, loss-free JSON form of a channel, for linking Sound Manager instances that live in
    different processes (SessionLink). Includes the mix-relevant parameters of every slot and,
    when `withMappers`, their learned value curves (what the AI needs to move them in real units).
*/
nlohmann::json channelToJson (const ChannelState&, bool withMappers, size_t maxParamsPerSlot = 32);
ChannelState channelFromJson (const nlohmann::json&);

/** Changes when a slot's plugin or parameter set changes (receivers cache mappers by it). */
std::string slotSignature (const SlotState&);

} // namespace smix::wire
