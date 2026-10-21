/*
 * mod-optimal-bot-raid API for other modules.
 *
 * AzerothCore puts every module's source folders on the include path, so another module can
 * `#include "OptimalBotRaidApi.h"` and call these. They are defined in cs_optimal_bot_raid.cpp,
 * so a module that uses them only links when mod-optimal-bot-raid is installed too.
 * Call them on the world thread (commands, world scripts), except DraftedBots.
 */

#ifndef OPTIMAL_BOT_RAID_API_H
#define OPTIMAL_BOT_RAID_API_H

#include "ObjectGuid.h"
#include <string>
#include <unordered_set>

class ChatHandler;
class Player;

namespace OptimalBotRaid
{
    // Same order as the module's own roles, so a Role sent as a number stays stable.
    enum class Role : uint8 { Tank, Healer, Melee, Ranged, Unknown };

    // The tank and healer slots of a raid lineup; the rest of the raid is damage.
    struct Lineup
    {
        uint32 tanks = 0;
        uint32 healers = 0;
    };

    // The lineup for a raid alias (`.botraid raids`, e.g. "mc", "bwl") at that size.
    // False when the raid has no lineup at that size.
    bool GetLineup(std::string const& raid, uint32 size, Lineup& lineup);

    // A group member's role as .botraid assemble sees it in a raid: from the active spec, and a
    // bear-tank bot counts as melee unless OptimalBotRaid.BearTanksInRaids is on.
    Role GetRaidRole(Player* member);

    // `.botraid assemble <raid> <size>` for the handler's player, with bots of minLevel-maxLevel.
    // Replies in the handler's chat like the command, cooldown and level limits included. False
    // only when the raid has no lineup; check the group afterwards to see what was drafted.
    bool Assemble(ChatHandler* handler, std::string const& raid, uint32 size, uint32 minLevel, uint32 maxLevel);

    // The random bots .botraid assemble drafted for this leader and that are still in the group.
    // Follows raid leadership; forgotten on a restart. Safe from any thread.
    std::unordered_set<ObjectGuid> DraftedBots(ObjectGuid leader);
}

#endif
