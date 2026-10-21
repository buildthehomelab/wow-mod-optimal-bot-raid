// Included in cs_optimal_bot_raid so the finder reuses the assembler and role model.
// Commands run on the world thread. No raw Player pointers survive a command.
struct FinderRaid
{
    char const* alias;
    uint32 map;
    char const* name;
    uint32 requiredProgression;
    uint32 meetingStoneSpawn;
};

static std::vector<FinderRaid> const& FinderRaids()
{
    static std::vector<FinderRaid> const raids = {
#ifdef EVERMORE_LFR_IP
        { "mc", 409, "Molten Core", PROGRESSION_START, 427 },
        { "bwl", 469, "Blackwing Lair", PROGRESSION_MOLTEN_CORE, 428 }
#endif
    };
    return raids;
}

static void FinderSend(Player* player, uint32 request, std::string const& payload)
{
    WorldPacket packet;
    ChatHandler::BuildChatPacket(packet, CHAT_MSG_WHISPER, LANG_ADDON,
        player->GetGUID(), player->GetGUID(), "EMLFR\t2\t" + std::to_string(request) + "\t" + payload,
        CHAT_TAG_NONE, player->GetName());
    player->GetSession()->SendPacket(&packet);
}

static bool FinderEnabled(FinderRaid const& raid)
{
    return sConfigMgr->GetOption<bool>("OptimalBotRaid.Finder.Enable", false) &&
        sConfigMgr->GetOption<bool>(std::string("OptimalBotRaid.Finder.") + raid.alias, false);
}

// Use the realm's loaded spawn coordinates; never accept a client destination.
// Default Blackrock stones: BRD approach for MC, Spire approach for BWL.
static GameObjectData const* FinderStone(FinderRaid const& raid)
{
    uint32 const spawn = sConfigMgr->GetOption<uint32>(
        std::string("OptimalBotRaid.Finder.") + raid.alias + ".MeetingStoneSpawn", raid.meetingStoneSpawn);
    auto const* stone = sObjectMgr->GetGameObjectData(spawn);
    if (!stone) return nullptr;
    auto const* object = sObjectMgr->GetGameObjectTemplate(stone->id);
    auto const* map = sMapStore.LookupEntry(stone->mapid);
    if (!object || object->type != GAMEOBJECT_TYPE_MEETINGSTONE || !map || map->Instanceable())
        return nullptr;
    return stone;
}

static bool FinderAtStone(Player* player, GameObjectData const& stone)
{
    float const dx = player->GetPositionX() - stone.posX;
    float const dy = player->GetPositionY() - stone.posY;
    float const dz = player->GetPositionZ() - stone.posZ;
    return player->GetMapId() == stone.mapid && dx * dx + dy * dy + dz * dz <= 400.0f;
}

static bool FinderTravelToStone(Player* player, GameObjectData const& stone)
{
    if (FinderAtStone(player, stone)) return true;
    // Land beside the object rather than inside its collision. Staging traversal
    // must verify this small offset against the realm's terrain/client patches.
    return player->TeleportTo(stone.mapid,
        stone.posX + 3.0f * std::cos(stone.orientation),
        stone.posY + 3.0f * std::sin(stone.orientation), stone.posZ + 0.5f, stone.orientation);
}

static std::string FinderGatherDrafts(Player* player, GameObjectData const& stone)
{
    std::unordered_set<ObjectGuid> drafted;
    { std::lock_guard<std::mutex> lock(s_draftedBotsLock);
      auto it = s_draftedBots.find(player->GetGUID());
      if (it != s_draftedBots.end()) drafted = it->second; }
    Group* group = player->GetGroup();
    if (!group) return "You need an assembled raid.";
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        if (Player* bot = ref->GetSource(); bot && drafted.count(bot->GetGUID()) &&
            PlayerbotsMgr::instance().GetPlayerbotAI(bot) && sRandomPlayerbotMgr.IsRandomBot(bot))
            if (!FinderTravelToStone(bot, stone))
                return bot->GetName() + ": could not be summoned. Some members may already be travelling; use Gather Bots at the stone to retry.";
    return "";
}

static BotRole FinderRole(Player* member)
{
    BotRole role; float competence; uint32 buffs;
    MapBotProfile(member, GetBotSpec(member, member->getClass()), role, competence, buffs);
    // Match the assembler: forbidden bear bots do not satisfy a raid tank slot.
    if (role == ROLE_TANK && member->getClass() == CLASS_DRUID &&
        PlayerbotsMgr::instance().GetPlayerbotAI(member) && !BotRaidConfigData::instance()->bearTanksInRaids)
        return ROLE_MELEE;
    return role;
}

// Persistent eligibility is shared by catalogue filtering and every action.
// Do not use transient combat/travel state to hide an otherwise unlocked raid.
static std::string FinderAccessProblem(Player* member, [[maybe_unused]] FinderRaid const& raid)
{
    if (!member || !member->IsInWorld()) return "A group member is offline; eligibility cannot be checked.";
#ifndef EVERMORE_LFR_IP
    return "Individual Progression support is required for Raid Finder.";
#else
    std::string const who = member->GetName() + ": ";
    if (!sIndividualProgression->enabled)
        return "Individual Progression must be enabled for Raid Finder.";
    if (member->GetLevel() < 60) return who + "requires level 60.";
    // Use IP's character-stage query and configured account exclusions/realm cap.
    // Stage zero is the starting tier; hasPassedProgression intentionally rejects zero.
    if (raid.requiredProgression && sIndividualProgression->isNormalAccount(member) &&
        !sIndividualProgression->hasPassedProgression(member, static_cast<ProgressionState>(raid.requiredProgression)))
        return who + "must complete Molten Core progression to enter Blackwing Lair.";
    if (!member->Satisfy(sObjectMgr->GetAccessRequirement(raid.map, RAID_DIFFICULTY_10MAN_NORMAL), raid.map, false))
        return who + "does not meet the raid's access requirements (level, quest or item).";
    return "";
#endif
}

static std::string FinderCatalogueProblem(Player* player, FinderRaid const& raid)
{
    if (!FinderEnabled(raid)) return "This raid is not enabled for Raid Finder testing.";
    if (!FinderStone(raid)) return "The raid has no valid outdoor meeting stone.";
    if (Group* group = player->GetGroup())
    {
        for (auto const& slot : group->GetMemberSlots())
            if (std::string problem = FinderAccessProblem(ObjectAccessor::FindConnectedPlayer(slot.guid), raid); !problem.empty())
                return problem;
        return "";
    }
    return FinderAccessProblem(player, raid);
}

static std::string FinderMemberProblem(Player* member, FinderRaid const& raid)
{
    if (std::string problem = FinderAccessProblem(member, raid); !problem.empty()) return problem;
    std::string const who = member->GetName() + ": ";
    if (!member->IsAlive()) return who + "must be alive.";
    if (member->IsInCombat()) return who + "is in combat.";
    if (member->IsBeingTeleported() || member->IsInFlight() || member->InBattleground() || member->InArena())
        return who + "is travelling or in PvP.";
    if (member->GetRaidDifficulty() != RAID_DIFFICULTY_10MAN_NORMAL)
        return who + "must select Normal raid difficulty.";
    if (member->GetMap()->Instanceable() && member->GetMapId() != raid.map)
        return who + "must leave the current instance first.";
    // A solo/party preflight is not entry: the assembler has yet to create the raid.
    // CannotEnter also deliberately rejects players ALREADY in the target map.
    // TeleportTo enforces the full checks when an actual entrant crosses maps.
    if (member->GetGroup() && member->GetGroup()->isRaidGroup() && member->GetMapId() != raid.map &&
        sMapMgr->PlayerCannotEnter(raid.map, member))
        return who + "cannot enter this raid (access, instance save or capacity).";
    return "";
}

// Only status is allowed during combat; actions recheck live group membership and saves.
static std::string FinderProblem(Player* player, FinderRaid const& raid, bool ready = false)
{
    if (!FinderEnabled(raid)) return "This raid is not enabled for Raid Finder testing.";
    auto const* lineup = BotRaidConfigData::instance()->FindEncounter(raid.alias);
    if (!lineup || !lineup->quotas.count(40)) return "The raid's 40-player lineup is unavailable.";
    if (!ready && !FinderStone(raid)) return "The raid has no valid outdoor meeting stone.";
    Group* group = player->GetGroup();
    if (group && group->GetMembersCount() > 40) return "The group is too large.";
    if (group && group->isLFGGroup()) return "Leave the Dungeon Finder group before preparing a raid.";
    auto const* leaderBind = sInstanceSaveMgr->PlayerGetBoundInstance(
        group ? group->GetLeaderGUID() : player->GetGUID(), raid.map, RAID_DIFFICULTY_10MAN_NORMAL);
    uint32 const leaderSave = leaderBind && leaderBind->save ? leaderBind->save->GetInstanceId() : 0;
    uint32 tanks = 0, healers = 0;
    bool botTank = false;
    auto check = [&](Player* member) -> std::string
    {
        if (std::string problem = FinderMemberProblem(member, raid); !problem.empty()) return problem;
        auto const* bind = sInstanceSaveMgr->PlayerGetBoundInstance(member->GetGUID(), raid.map, RAID_DIFFICULTY_10MAN_NORMAL);
        if (bind && bind->perm && bind->save && bind->save->GetInstanceId() != leaderSave)
            return member->GetName() + ": has a different raid save. Raid Finder never clears saves.";
        BotRole role = FinderRole(member);
        if (role == ROLE_TANK) { ++tanks; botTank |= PlayerbotsMgr::instance().GetPlayerbotAI(member) != nullptr; }
        if (role == ROLE_HEALER) ++healers;
        if (ready && (member->GetMapId() != raid.map || member->GetInstanceId() != player->GetInstanceId()))
            return member->GetName() + ": has not arrived in this instance.";
        return "";
    };
    if (group)
    {
        // MemberSlots includes disconnected members, unlike GroupReference iteration.
        for (auto const& slot : group->GetMemberSlots())
            if (std::string problem = check(ObjectAccessor::FindConnectedPlayer(slot.guid)); !problem.empty()) return problem;
    }
    else if (std::string problem = check(player); !problem.empty()) return problem;
    if (ready)
    {
        if (!group || group->GetMembersCount() != 40) return "Recruitment is incomplete: 40 members are required.";
        auto const& q = lineup->quotas.at(40);
        if (!botTank || tanks < uint32(q.tanks) || healers < uint32(q.healers))
            return "The lineup is short of tanks or healers. A bot tank must lead the run.";
    }
    return "";
}

static bool HandleFinder(ChatHandler* handler, std::string action, Optional<std::string> optRaid, Optional<uint32> optRequest = {})
{
    Player* player = handler->GetSession()->GetPlayer();
    uint32 const request = optRequest ? *optRequest : 0;
    auto send = [&](std::string const& payload) { FinderSend(player, request, payload); };
    if (action == "catalog")
    {
        send("BEGIN");
        for (auto const& raid : FinderRaids())
        {
            auto const* lineup = BotRaidConfigData::instance()->FindEncounter(raid.alias);
            if (!lineup || !lineup->quotas.count(40) || !FinderCatalogueProblem(player, raid).empty()) continue;
            auto const& q = lineup->quotas.at(40);
            send("RAID\t" + std::string(raid.alias) + "\t" + raid.name + "\t" +
                std::to_string(raid.map) + "\t" + std::to_string(q.tanks) + "\t" +
                std::to_string(q.healers) + "\t" + std::to_string(40 - q.tanks - q.healers) + "\t" +
                (FinderEnabled(raid) ? "1" : "0"));
        }
        send("END");
        return true;
    }
    FinderRaid const* selected = nullptr;
    for (auto const& raid : FinderRaids()) if (optRaid && *optRaid == raid.alias) selected = &raid;
    auto reply = [&](bool ok, std::string const& message)
    {
        send("RESULT\t" + action + "\t" + (selected ? selected->alias : "-") + "\t" +
            (ok ? "1" : "0") + "\t" + message);
        return true;
    };
    if (!selected) return reply(false, "Select a raid first.");
    FinderRaid const& raid = *selected;
    Group* group = player->GetGroup();
    if (action != "status" && action != "summon" && group && !group->IsLeader(player->GetGUID()))
        return reply(false, "Only the raid leader can prepare or control the raid.");
    if (action == "status")
    {
        auto const* bind = sInstanceSaveMgr->PlayerGetBoundInstance(player->GetGUID(), raid.map, RAID_DIFFICULTY_10MAN_NORMAL);
        std::string save = "No personal raid save.";
        if (bind && bind->save)
            save = "Saved instance " + std::to_string(bind->save->GetInstanceId()) + "; resets " +
                std::to_string(bind->save->GetResetTime()) + ".";
        send("STATUS\t" + std::string(raid.alias) + "\t" +
            std::to_string(group ? group->GetMembersCount() : 1) + "\t" +
            std::to_string(FinderRole(player)) + "\t" + save);
        std::string problem = FinderProblem(player, raid);
        return reply(problem.empty(), problem.empty() ? "Ready to prepare. " + save : problem);
    }
    if (action != "assemble" && action != "summon" && action != "rally" && action != "ready")
        return reply(false, "Unknown Raid Finder action.");
    if (std::string problem = FinderProblem(player, raid, action == "ready"); !problem.empty())
        return reply(false, problem);
    if (action == "assemble")
    {
        if (player->GetMap()->Instanceable())
            return reply(false, "Leave the instance before recruiting through Raid Finder; groups gather at the outdoor meeting stone.");
        if (FinderRole(player) == ROLE_TANK)
            return reply(false, "Autonomous runs currently support human healers and damage dealers. A bot tank leads.");
        ExecuteAssemble(handler, 60, 60, true, 40, BotRaidConfigData::instance()->FindEncounter(raid.alias));
        group = player->GetGroup();
        if (!group || group->GetMembersCount() != 40)
            return reply(false, "Recruitment is incomplete. See chat for availability and cooldown details; retry or dismiss drafted bots.");
        // Check roles before offering entry, even though the assembler may have filled scarce roles with DPS.
        auto const& quota = BotRaidConfigData::instance()->FindEncounter(raid.alias)->quotas.at(40);
        uint32 tanks = 0, healers = 0; bool botTank = false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
            {
                BotRole role = FinderRole(member);
                if (role == ROLE_TANK) { ++tanks; botTank |= PlayerbotsMgr::instance().GetPlayerbotAI(member) != nullptr; }
                if (role == ROLE_HEALER) ++healers;
            }
        if (!botTank || tanks < uint32(quota.tanks) || healers < uint32(quota.healers))
            return reply(false, "Recruitment is short of tanks or healers. Dismiss drafted bots and retry later.");
        if (std::string problem = FinderProblem(player, raid); !problem.empty()) return reply(false, problem);
        return reply(true, "Raid assembled. Confirm Summon to Stone to gather outside; each human confirms their own travel.");
    }
    if (action == "summon")
    {
        if (!group || !group->isRaidGroup() || group->GetMembersCount() != 40)
            return reply(false, "The raid leader must assemble the complete roster first.");
        auto const* stone = FinderStone(raid);
        if (!stone) return reply(false, "The raid has no valid outdoor meeting stone.");
        if (!FinderTravelToStone(player, *stone))
            return reply(false, "Summon refused by the server. No instance entry was attempted.");
        // Only the leader moves the tracked random-bot drafts. Friends confirm
        // their own travel; manually invited bots and player alts are untouched.
        if (group->IsLeader(player->GetGUID()))
            if (std::string problem = FinderGatherDrafts(player, *stone); !problem.empty()) return reply(false, problem);
        return reply(true, "Summoning to the outdoor meeting stone. Gather there, then use the normal raid entrance.");
    }
    if (action == "rally")
    {
        auto const* stone = FinderStone(raid);
        if (!stone || !FinderAtStone(player, *stone))
            return reply(false, "Stand at the selected raid's outdoor meeting stone to Gather Bots.");
        if (std::string problem = FinderGatherDrafts(player, *stone); !problem.empty()) return reply(false, problem);
        return reply(true, "Drafted bots summoned to the meeting stone. Enter the raid together through its normal entrance.");
    }
    return reply(true, "The roster is ready in this instance.");
}
