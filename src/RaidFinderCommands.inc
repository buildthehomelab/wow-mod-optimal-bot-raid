// Included in cs_optimal_bot_raid so the finder reuses the assembler and role model.
// Commands run on the world thread. No raw Player pointers survive a command.
struct FinderRaid
{
    char const* alias;
    uint32 map;
    char const* name;
};

static std::vector<FinderRaid> const& FinderRaids()
{
    static std::vector<FinderRaid> const raids = {
        { "mc", 409, "Molten Core" }, { "bwl", 469, "Blackwing Lair" }
    };
    return raids;
}

static void FinderSend(Player* player, std::string const& payload)
{
    WorldPacket packet;
    ChatHandler::BuildChatPacket(packet, CHAT_MSG_WHISPER, LANG_ADDON,
        player->GetGUID(), player->GetGUID(), "EMLFR\t1\t" + payload,
        CHAT_TAG_NONE, player->GetName());
    player->GetSession()->SendPacket(&packet);
}

static bool FinderEnabled(FinderRaid const& raid)
{
    return sConfigMgr->GetOption<bool>("OptimalBotRaid.Finder.Enable", false) &&
        sConfigMgr->GetOption<bool>(std::string("OptimalBotRaid.Finder.") + raid.alias, false);
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

static std::string FinderMemberProblem(Player* member, FinderRaid const& raid)
{
    if (!member || !member->IsInWorld()) return "A group member is offline.";
    std::string const who = member->GetName() + ": ";
    if (!member->IsAlive()) return who + "must be alive.";
    if (member->IsInCombat()) return who + "is in combat.";
    if (member->IsBeingTeleported() || member->IsInFlight() || member->InBattleground() || member->InArena())
        return who + "is travelling or in PvP.";
    if (member->GetLevel() < 60) return who + "requires level 60.";
    if (member->GetRaidDifficulty() != RAID_DIFFICULTY_10MAN_NORMAL)
        return who + "must select Normal raid difficulty.";
    if (member->GetMap()->Instanceable() && member->GetMapId() != raid.map)
        return who + "must leave the current instance first.";
#ifdef EVERMORE_LFR_IP
    if (raid.map == 469 && sIndividualProgression->enabled &&
        sIndividualProgression->isNormalAccount(member) &&
        !sIndividualProgression->hasPassedProgression(member, PROGRESSION_MOLTEN_CORE))
        return who + "must complete Molten Core progression to enter Blackwing Lair.";
#endif
    if (!member->Satisfy(sObjectMgr->GetAccessRequirement(raid.map, RAID_DIFFICULTY_10MAN_NORMAL), raid.map, false))
        return who + "does not meet the raid's access requirements (level, quest or item).";
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

static bool HandleFinder(ChatHandler* handler, std::string action, Optional<std::string> optRaid)
{
    Player* player = handler->GetSession()->GetPlayer();
    if (action == "catalog")
    {
        FinderSend(player, "BEGIN");
        for (auto const& raid : FinderRaids())
        {
            auto const* lineup = BotRaidConfigData::instance()->FindEncounter(raid.alias);
            if (!lineup || !lineup->quotas.count(40)) continue;
            auto const& q = lineup->quotas.at(40);
            FinderSend(player, "RAID\t" + std::string(raid.alias) + "\t" + raid.name + "\t" +
                std::to_string(raid.map) + "\t" + std::to_string(q.tanks) + "\t" +
                std::to_string(q.healers) + "\t" + std::to_string(40 - q.tanks - q.healers) + "\t" +
                (FinderEnabled(raid) ? "1" : "0"));
        }
        FinderSend(player, "END");
        return true;
    }
    FinderRaid const* selected = nullptr;
    for (auto const& raid : FinderRaids()) if (optRaid && *optRaid == raid.alias) selected = &raid;
    auto reply = [&](bool ok, std::string const& message)
    {
        FinderSend(player, "RESULT\t" + action + "\t" + (selected ? selected->alias : "-") + "\t" +
            (ok ? "1" : "0") + "\t" + message);
        return true;
    };
    if (!selected) return reply(false, "Select a raid first.");
    FinderRaid const& raid = *selected;
    Group* group = player->GetGroup();
    if (action != "status" && action != "enter" && group && !group->IsLeader(player->GetGUID()))
        return reply(false, "Only the raid leader can prepare or control the raid.");
    if (action == "status")
    {
        auto const* bind = sInstanceSaveMgr->PlayerGetBoundInstance(player->GetGUID(), raid.map, RAID_DIFFICULTY_10MAN_NORMAL);
        std::string save = "No personal raid save.";
        if (bind && bind->save)
            save = "Saved instance " + std::to_string(bind->save->GetInstanceId()) + "; resets " +
                std::to_string(bind->save->GetResetTime()) + ".";
        FinderSend(player, "STATUS\t" + std::string(raid.alias) + "\t" +
            std::to_string(group ? group->GetMembersCount() : 1) + "\t" +
            std::to_string(FinderRole(player)) + "\t" + save);
        std::string problem = FinderProblem(player, raid);
        return reply(problem.empty(), problem.empty() ? "Ready to prepare. " + save : problem);
    }
    if (action != "assemble" && action != "enter" && action != "rally" && action != "ready")
        return reply(false, "Unknown Raid Finder action.");
    if (std::string problem = FinderProblem(player, raid, action == "ready"); !problem.empty())
        return reply(false, problem);
    if (action == "assemble")
    {
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
        return reply(true, "Raid assembled. Each human player must confirm Enter Raid in their own menu.");
    }
    if (action == "enter")
    {
        if (!group || !group->isRaidGroup() || group->GetMembersCount() != 40)
            return reply(false, "The raid leader must assemble the complete roster first.");
        Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
        if (!leader || !leader->IsInWorld() || leader->IsBeingTeleported())
            return reply(false, "Wait for the raid leader to finish travelling.");
        if (leader != player && leader->GetMapId() != raid.map)
            return reply(false, "The raid leader must enter first to establish the shared instance.");
        if (player->GetMapId() == raid.map)
        {
            if (leader->GetMapId() == raid.map && player->GetInstanceId() == leader->GetInstanceId())
                return reply(true, "You are already in the leader's instance. The leader can bring the bots.");
            return reply(false, "You are in another copy of this raid. Leave that instance, then Enter Raid again.");
        }
        AreaTriggerTeleport const* entrance = sObjectMgr->GetMapEntranceTrigger(raid.map);
        if (!entrance || entrance->target_mapId != raid.map) return reply(false, "The server has no entrance for this raid.");
        // TeleportTo runs the progression module's hooks and the core's entry checks.
        if (!player->TeleportTo(raid.map, entrance->target_X, entrance->target_Y, entrance->target_Z, entrance->target_Orientation, 0, leader == player ? nullptr : leader))
            return reply(false, "Entry refused by the server. Check chat for progression or access requirements.");
        return reply(true, "Entering raid. After arrival, the leader can bring the bots.");
    }
    if (action == "rally")
    {
        if (!group || player->GetMapId() != raid.map) return reply(false, "Enter the selected raid first.");
        std::unordered_set<ObjectGuid> drafted;
        { std::lock_guard<std::mutex> lock(s_draftedBotsLock);
          auto it = s_draftedBots.find(player->GetGUID());
          if (it != s_draftedBots.end()) drafted = it->second; }
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* bot = ref->GetSource(); bot && drafted.count(bot->GetGUID()) &&
                PlayerbotsMgr::instance().GetPlayerbotAI(bot) && sRandomPlayerbotMgr.IsRandomBot(bot) &&
                (bot->GetMapId() != raid.map || bot->GetInstanceId() != player->GetInstanceId()))
                if (!bot->TeleportTo(raid.map, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                    player->GetOrientation(), 0, player))
                    return reply(false, bot->GetName() + ": could not enter. No saves were cleared.");
        return reply(true, "Bots called to your instance. Wait for arrivals, then Start Run.");
    }
    return reply(true, "The roster is ready in this instance.");
}
