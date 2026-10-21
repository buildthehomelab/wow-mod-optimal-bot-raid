// Contract harness: compile and execute the production command implementation
// against a small in-memory core, including entry refusals and roster mutations.
#include <cassert>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using uint32 = uint32_t;
using ObjectGuid = uint32;
template<class T> using Optional = std::optional<T>;
constexpr int RAID_DIFFICULTY_10MAN_NORMAL = 0, CLASS_DRUID = 11;
constexpr int CHAT_MSG_WHISPER = 1, LANG_ADDON = 2, CHAT_TAG_NONE = 0;
enum BotRole { ROLE_TANK, ROLE_HEALER, ROLE_MELEE, ROLE_RANGED, ROLE_UNKNOWN };
struct WorldPacket { std::string data; };
struct Session { std::string last; std::vector<std::string> packets; void SendPacket(WorldPacket* p) { last = p->data; packets.push_back(last); } };
struct Map { bool instance = false; bool Instanceable() const { return instance; } };
struct Group;
struct Player {
    uint32 id, map = 0, instance = 0, level = 60, cls = 1, stage = 1;
    bool alive = true, combat = false, moving = false, world = true, bot = false, random = false, access = true, teleport = true, excluded = false;
    BotRole role = ROLE_RANGED;
    Group* group = nullptr;
    Session session; Map location;
    std::string GetName() const { return "Player" + std::to_string(id); }
    ObjectGuid GetGUID() const { return id; }
    bool IsInWorld() const { return world; }
    bool IsAlive() const { return alive; }
    bool IsInCombat() const { return combat; }
    bool IsBeingTeleported() const { return moving; }
    bool IsInFlight() const { return false; }
    bool InBattleground() const { return false; }
    bool InArena() const { return false; }
    uint32 GetLevel() const { return level; }
    int GetRaidDifficulty() const { return 0; }
    Map* GetMap() { location.instance = map != 0; return &location; }
    uint32 GetMapId() const { return map; }
    uint32 GetInstanceId() const { return instance; }
    uint32 getClass() const { return cls; }
    Group* GetGroup() const { return group; }
    Session* GetSession() { return &session; }
    bool Satisfy(void const*, uint32, bool) const { return access; }
    float GetPositionX() const { return 1; } float GetPositionY() const { return 2; }
    float GetPositionZ() const { return 3; } float GetOrientation() const { return 0; }
    bool TeleportTo(uint32 target, float, float, float, float, int = 0, Player* owner = nullptr) {
        if (!teleport) return false;
        map = target; instance = owner ? owner->instance : 10; return true;
    }
};
struct GroupReference {
    Player* player; GroupReference* following = nullptr;
    Player* GetSource() { return player; }
    GroupReference* next() { return following; }
};
struct Slot { ObjectGuid guid; };
struct Group {
    uint32 leader = 1; bool raid = true, lfg = false;
    std::vector<Slot> slots; std::vector<GroupReference> refs;
    uint32 GetMembersCount() const { return slots.size(); }
    bool isRaidGroup() const { return raid; } bool isLFGGroup() const { return lfg; }
    bool IsLeader(ObjectGuid g) const { return leader == g; }
    ObjectGuid GetLeaderGUID() const { return leader; }
    auto const& GetMemberSlots() const { return slots; }
    GroupReference* GetFirstMember() {
        for (size_t i = 0; i < refs.size(); ++i) refs[i].following = i + 1 < refs.size() ? &refs[i+1] : nullptr;
        return refs.empty() ? nullptr : &refs[0];
    }
    void Add(Player& p) { slots.push_back({p.id}); refs.push_back({&p}); p.group = this; }
};
std::unordered_map<ObjectGuid, Player*> players;
namespace ObjectAccessor { Player* FindConnectedPlayer(ObjectGuid id) { auto i = players.find(id); return i == players.end() ? nullptr : i->second; } }
struct InstanceSave { uint32 id = 10; uint32 GetInstanceId() const { return id; } time_t GetResetTime() const { return 100; } };
struct Bind { InstanceSave* save = nullptr; bool perm = false; };
struct Saves {
    std::map<ObjectGuid, Bind> binds;
    Bind* PlayerGetBoundInstance(ObjectGuid id, uint32, int) { auto it = binds.find(id); return it == binds.end() ? nullptr : &it->second; }
} saves;
auto* sInstanceSaveMgr = &saves;
struct AreaTriggerTeleport { uint32 target_mapId = 409; float target_X = 0, target_Y = 0, target_Z = 0, target_Orientation = 0; };
struct Objects {
    AreaTriggerTeleport entrance;
    void const* GetAccessRequirement(uint32, int) { return nullptr; }
    AreaTriggerTeleport const* GetMapEntranceTrigger(uint32 map) { entrance.target_mapId = map; return &entrance; }
} objects;
auto* sObjectMgr = &objects;
struct Maps {
    int calls = 0;
    bool PlayerCannotEnter(uint32 map, Player* p) { ++calls; return !p->group || !p->group->raid || p->map == map || !p->access; }
} maps;
auto* sMapMgr = &maps;
struct Config { bool enabled = true; template<class T> T GetOption(std::string const&, T) { return enabled; } } config;
auto* sConfigMgr = &config;
struct PlayerbotsMgr {
    static PlayerbotsMgr& instance() { static PlayerbotsMgr m; return m; }
    void* GetPlayerbotAI(Player* p) { return p->bot ? p : nullptr; }
};
struct RandomBots { bool IsRandomBot(Player* p) { return p->random; } } sRandomPlayerbotMgr;
struct BotRaidConfigData {
    struct Quota { int tanks = 2, healers = 3; };
    struct EncounterLineup { std::map<uint32, Quota> quotas = {{40, {}}}; } lineup;
    bool bearTanksInRaids = false;
    static BotRaidConfigData* instance() { static BotRaidConfigData c; return &c; }
    EncounterLineup const* FindEncounter(std::string const&) const { return &lineup; }
};
std::unordered_map<ObjectGuid, std::unordered_set<ObjectGuid>> s_draftedBots;
std::mutex s_draftedBotsLock;
#include "../src/BotRaidDraftOwnership.inc"
#ifdef EVERMORE_LFR_IP
enum ProgressionState : uint32 { PROGRESSION_START = 0, PROGRESSION_MOLTEN_CORE = 1 };
struct Progression {
    bool enabled = true;
    bool isNormalAccount(Player* p) const { return !p->random && !p->excluded; }
    bool hasPassedProgression(Player* p, ProgressionState stage) const { return enabled && stage && p->stage >= stage; }
} progression;
auto* sIndividualProgression = &progression;
#endif
struct ChatHandler {
    Player* player;
    struct Holder { Player* p; Player* GetPlayer() { return p; } } holder;
    Holder* GetSession() { holder.p = player; return &holder; }
    static void BuildChatPacket(WorldPacket& p, int, int, ObjectGuid, ObjectGuid, std::string const& text, int, std::string const&) { p.data = text; }
};
struct Commands {
    static uint32 GetBotSpec(Player*, uint32) { return 0; }
    static void MapBotProfile(Player* p, uint32, BotRole& r, float&, uint32&) { r = p->role; }
    inline static int assembleCalls = 0;
    static bool ExecuteAssemble(ChatHandler*, int, int, bool, int, BotRaidConfigData::EncounterLineup const*) { ++assembleCalls; return true; }
#include "../src/RaidFinderCommands.inc"
};
int main() {
    Player leader{1}; players[1] = &leader;
    ChatHandler handler{&leader};
    auto catalogue = [&]() {
        leader.session.packets.clear();
        Commands::HandleFinder(&handler, "catalog", {});
        std::string result;
        for (auto const& packet : leader.session.packets) result += packet + "\n";
        return result;
    };
#ifndef EVERMORE_LFR_IP
    assert(Commands::FinderRaids().empty());
    assert(catalogue().find("RAID\t") == std::string::npos);
    Commands::HandleFinder(&handler, "assemble", std::string("bwl"));
    assert(Commands::assembleCalls == 0);
    std::cout << "PASS: missing Individual Progression offers no raids or assembly\n";
    return 0;
#endif
    auto raid = Commands::FinderRaids()[0];
    config.enabled = false;
    assert(!Commands::FinderProblem(&leader, raid).empty());
    config.enabled = true;
    assert(Commands::FinderProblem(&leader, raid).empty()); // solo may prepare
    assert(maps.calls == 0); // must not ask the core to enter an uncreated raid
#ifdef EVERMORE_LFR_IP
    progression.enabled = false;
    assert(catalogue().find("RAID\t") == std::string::npos);
    assert(Commands::FinderProblem(&leader, raid).find("must be enabled") != std::string::npos);
    progression.enabled = true;
    leader.stage = 0;
    assert(catalogue().find("RAID\tmc\t") != std::string::npos);
    assert(catalogue().find("RAID\tbwl\t") == std::string::npos);
    for (auto const& action : {"assemble", "enter", "rally", "ready"}) {
        Commands::HandleFinder(&handler, action, std::string("bwl"));
        assert(leader.session.last.find("complete Molten Core") != std::string::npos);
    }
    assert(Commands::assembleCalls == 0 && leader.map == 0);
    assert(Commands::FinderProblem(&leader, Commands::FinderRaids()[1]).find("complete Molten Core") != std::string::npos);
    leader.stage = 1;
    assert(Commands::FinderProblem(&leader, Commands::FinderRaids()[1]).empty());
    assert(catalogue().find("RAID\tbwl\t") != std::string::npos);
    leader.stage = 18;
    assert(catalogue().find("RAID\tmc\t") != std::string::npos); // prior tiers remain unlocked
    leader.level = 59;
    assert(catalogue().find("RAID\t") == std::string::npos);
    leader.level = 60; leader.access = false;
    assert(catalogue().find("RAID\t") == std::string::npos);
    leader.access = true; leader.stage = 1;
#endif
    Group group; group.Add(leader);
    group.raid = false;
    assert(Commands::FinderProblem(&leader, raid).empty()); // ordinary party may prepare
    group.raid = true;
#ifdef EVERMORE_LFR_IP
    Player friendAtStart{99}; friendAtStart.stage = 0; players[99] = &friendAtStart;
    group.Add(friendAtStart);
    assert(catalogue().find("RAID\tbwl\t") == std::string::npos);
    assert(!Commands::FinderProblem(&leader, Commands::FinderRaids()[1]).empty());
    // Alt bots on normal accounts keep their character gates; random/excluded accounts follow IP policy.
    friendAtStart.bot = true;
    assert(catalogue().find("RAID\tbwl\t") == std::string::npos);
    friendAtStart.random = true;
    assert(catalogue().find("RAID\tbwl\t") != std::string::npos);
    friendAtStart.random = false; friendAtStart.excluded = true;
    assert(catalogue().find("RAID\tbwl\t") != std::string::npos);
    players.erase(99);
    assert(catalogue().find("RAID\t") == std::string::npos); // unknown member is not assumed eligible
    group.slots.pop_back(); group.refs.pop_back();
#endif
    std::vector<Player> bots; bots.reserve(39);
    for (uint32 id = 2; id <= 40; ++id) {
        bots.push_back(Player{id}); auto& bot = bots.back(); bot.bot = bot.random = true;
        bot.role = id <= 3 ? ROLE_TANK : id <= 6 ? ROLE_HEALER : ROLE_RANGED;
        players[id] = &bot; group.Add(bot); s_draftedBots[1].insert(id);
    }
    assert(Commands::FinderProblem(&leader, raid).empty());
    assert(!Commands::FinderProblem(&leader, raid, true).empty()); // not arrived
    // A friend cannot create a separate unsaved copy before the leader arrives.
    bots.back().bot = bots.back().random = false;
    s_draftedBots[1].erase(40);
    ChatHandler friendHandler{&bots.back()};
    Commands::HandleFinder(&friendHandler, "enter", std::string("mc"));
    assert(bots.back().map == 0 && bots.back().session.last.find("leader must enter first") != std::string::npos);
    Commands::HandleFinder(&handler, "enter", std::string("mc"));
    assert(leader.map == 409 && leader.instance == 10);
    Commands::HandleFinder(&friendHandler, "enter", std::string("mc"));
    assert(bots.back().map == 409 && bots.back().instance == leader.instance);
    bots.back().instance = 999;
    Commands::HandleFinder(&friendHandler, "enter", std::string("mc"));
    assert(bots.back().session.last.find("another copy") != std::string::npos);
    bots.back().instance = leader.instance;
    Commands::HandleFinder(&handler, "rally", std::string("mc"));
    assert(leader.session.last.find("RESULT\trally\tmc\t1") != std::string::npos);
    assert(Commands::FinderProblem(&leader, raid, true).empty()); // already-in-map is not entry
    bots[0].combat = true;
    assert(!Commands::FinderProblem(&leader, raid, true).empty()); bots[0].combat = false;
    bots[0].world = false;
    assert(!Commands::FinderProblem(&leader, raid).empty()); bots[0].world = true;
    bots[0].role = ROLE_RANGED;
    assert(!Commands::FinderProblem(&leader, raid, true).empty()); bots[0].role = ROLE_TANK;
    bots[0].cls = CLASS_DRUID;
    assert(!Commands::FinderProblem(&leader, raid, true).empty()); bots[0].cls = 1;
    InstanceSave leaderSave{10}, otherSave{20};
    saves.binds[1] = {&leaderSave, true}; saves.binds[2] = {&otherSave, true};
    assert(Commands::FinderProblem(&leader, raid).find("different raid save") != std::string::npos);
    saves.binds.clear();
    players.erase(40);
    assert(!Commands::FinderProblem(&leader, raid).empty()); players[40] = &bots.back();
    ChatHandler follower{&bots.back()};
    Commands::HandleFinder(&follower, "assemble", std::string("mc"));
    assert(bots.back().session.last.find("Only the raid leader") != std::string::npos);
    bots[0].map = bots[0].instance = 0; s_draftedBots[1].erase(2);
    Commands::HandleFinder(&handler, "rally", std::string("mc"));
    assert(bots[0].map == 0); // never teleport a manually invited bot
    s_draftedBots[1].insert(2); bots[0].teleport = false;
    Commands::HandleFinder(&handler, "rally", std::string("mc"));
    assert(leader.session.last.find("could not enter") != std::string::npos);
    // Transfer the actual production ownership ledger to a new leader; rally
    // must use that ledger even after the former owner's entry is forgotten.
    TransferDraftOwnership(1, 40);
    group.leader = 40;
    assert(!s_draftedBots.count(1) && s_draftedBots[40].count(2));
    bots[0].teleport = true;
    Commands::HandleFinder(&friendHandler, "rally", std::string("mc"));
    assert(bots[0].map == 409 && bots[0].instance == bots.back().instance);
    TransferDraftOwnership(40, 1); group.leader = 1;
    leader.map = leader.instance = 0; leader.teleport = false;
    Commands::HandleFinder(&handler, "enter", std::string("mc"));
    assert(leader.map == 0 && leader.session.last.find("Entry refused") != std::string::npos);
    std::cout << "PASS: disabled, solo preflight, arrival, rally, combat, offline, role shortage, bears, saves, leadership, ownership, teleport refusal\n";
}
