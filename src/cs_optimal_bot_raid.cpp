/*
 * AzerothCore Module: Optimal Bot Raid
 * Solves Set Cover + Knapsack for perfectly scaled bot raids.
 * FIX: Thread-safe player iteration, MotionMaster stack preservation, 
 * EXPLICIT AI Master reset, Strategy Cleansing, and File-based Debug Logging.
 * REFACTOR: Dynamic Heuristic extracted to cached memory with tight bounds validation.
 * FEATURE: Non-blocking, in-memory string-buffered telemetry for algorithmic validation.
 * UPDATE: Captures complete bot state transitions during Assembly and Dismissal. Debug deprecated.
 * UPDATE: Added custom level ranges, auto-relaxation down to level 10, and freeroam dismissal support.
 * HOTFIX: Native ChatCommandTable method overloading to fix AC's unsigned int strict-type parsing.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Player.h"
#include "Group.h"
#include "GroupMgr.h"
#include "ObjectAccessor.h"
#include "MotionMaster.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "PlayerbotFactory.h"
#include "Config.h"
#include "Map.h"
#include "LFGMgr.h"
#include "DBCStores.h"
#include "InstanceSaveMgr.h"
#include "World.h"
#include "GameTime.h"
#include "ObjectMgr.h"
#include "MapMgr.h"
#include "WorldPacket.h"
#if __has_include("IndividualProgression.h")
#include "IndividualProgression.h"
#define EVERMORE_LFR_IP 1
#endif
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <ctime>
#include <shared_mutex>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "Log.h"

using namespace Acore::ChatCommands;

// Global Telemetry State
static bool s_telemetryEnabled = false;

// Thick Hide (rank 3): mod-playerbots runs a feral druid bot that has it as a bear tank, the rest
// as cats. Says nothing about how a human plays.
constexpr uint32 SPELL_DRUID_THICK_HIDE = 16931;

// Bots drafted by .botraid assemble, per leader. Dismiss only releases these. The group hooks
// that prune it can run on map threads, so every access takes the lock.
static std::unordered_map<ObjectGuid, std::unordered_set<ObjectGuid>> s_draftedBots;
static std::mutex s_draftedBotsLock;

// When each player last moved bots with .botraid assemble / dismiss. Only the commands touch
// these, and they run on the world thread.
static std::unordered_map<ObjectGuid, Seconds> s_lastAssemble;
static std::unordered_map<ObjectGuid, Seconds> s_lastDismiss;

// ==============================================================================
// CONFIGURATION CACHE & EXPLICIT BOUNDARY VALIDATION
// ==============================================================================

enum BotRole { ROLE_TANK, ROLE_HEALER, ROLE_MELEE, ROLE_RANGED, ROLE_UNKNOWN };

struct BotRaidConfigData {
    float weightGS;
    float weightBuff;
    float buffBase;
    float buffBloodlust;
    float buffReplenishment;

    float aiMult[12][3]; // Index by [Class ID][Tree]

    int32 maxLevelAbovePlayer;
    bool sameLevelAtCaps;
    bool raidBotLevel;
    bool sortGroups;
    bool bearTanksInRaids;
    uint32 commandCooldown;

    // The class minimums (hunters to mages) are the fewest of that class to bring in that role's
    // slots; the class caps (paladinsMax, shamansMax) the most outside the tank slots, -1 for no
    // cap.
    struct Quota {
        int tanks; int healers; int melee;
        int hunters = 0; int warlocks = 0; int mages = 0;
        int warriorTanks = 0; int priests = 0; int druidHealers = 0; int rogues = 0; int warriors = 0;
        int paladinsMax = -1; int shamansMax = -1;
    };

    // The classes a quota can ask a minimum of, in the order they claim their role's slots:
    // warriors tank (the lineups leave tanks to warriors and paladins alike), priests heal and
    // bring Fear Ward and dispels, druids Innervate and Rebirth, rogues and warriors deal
    // physical damage (bosses like Chromaggus shrug off most spell schools), hunters for
    // Tranquilizing Shot bosses, warlocks for the curses and banishes, mages for decursing,
    // sheep and AoE.
    struct ClassMinimum { BotRole role; uint8 cls; int Quota::*count; char const* configKey; char const* singular; char const* plural; };
    static constexpr ClassMinimum CLASS_MINIMUMS[] = {
        { ROLE_TANK,   CLASS_WARRIOR, &Quota::warriorTanks, "WarriorTanksMin",  "warrior tank",  "warrior tanks"  },
        { ROLE_HEALER, CLASS_PRIEST,  &Quota::priests,      "PriestsMin",       "priest healer", "priest healers" },
        { ROLE_HEALER, CLASS_DRUID,   &Quota::druidHealers, "DruidHealersMin",  "druid healer",  "druid healers"  },
        { ROLE_MELEE,  CLASS_ROGUE,   &Quota::rogues,       "RoguesMin",        "rogue",         "rogues"         },
        { ROLE_MELEE,  CLASS_WARRIOR, &Quota::warriors,     "MeleeWarriorsMin", "melee warrior", "melee warriors" },
        { ROLE_RANGED, CLASS_HUNTER,  &Quota::hunters,      "HuntersMin",       "hunter",        "hunters"        },
        { ROLE_RANGED, CLASS_WARLOCK, &Quota::warlocks,     "WarlocksMin",      "warlock",       "warlocks"       },
        { ROLE_RANGED, CLASS_MAGE,    &Quota::mages,        "MagesMin",         "mage",          "mages"          },
    };

    // Classes a quota can cap, so they don't crowd the others out: the bots' scores favor
    // paladin and shaman healers. Tanks don't count: a paladin tank is as good as a warrior.
    struct ClassCap { uint8 cls; int Quota::*count; char const* configKey; char const* singular; char const* plural; };
    static constexpr ClassCap CLASS_CAPS[] = {
        { CLASS_PALADIN, &Quota::paladinsMax, "PaladinsMax", "paladin", "paladins" },
        { CLASS_SHAMAN,  &Quota::shamansMax,  "ShamansMax",  "shaman",  "shamans"  },
    };

    // "at least 5 priest healers, 2 hunters; at most 6 paladins, 6 shamans"; empty without any.
    static std::string DescribeClassMix(Quota const& q) {
        std::string least, most;
        for (ClassMinimum const& m : CLASS_MINIMUMS)
            if (int count = q.*m.count)
                least += (least.empty() ? "" : ", ") + std::to_string(count) + " " + (count == 1 ? m.singular : m.plural);
        for (ClassCap const& c : CLASS_CAPS)
            if (int count = q.*c.count; count >= 0)
                most += (most.empty() ? "" : ", ") + std::to_string(count) + " " + (count == 1 ? c.singular : c.plural);
        std::string text = least.empty() ? "" : "at least " + least;
        if (!most.empty())
            text += (text.empty() ? "" : "; ") + std::string("at most ") + most;
        return text;
    }
    Quota quotas[7]; // Indexes: 0=5, 1=10, 2=15, 3=20, 4=25, 5=40Vanilla, 6=40WotLK

    // Raid-specific lineups, used inside the raid's map or with .botraid assemble <alias> <size>.
    struct EncounterLineup {
        std::string name;                  // shown to the player
        std::string configKey;             // OptimalBotRaid.Quota.<configKey>.<size>.*
        std::vector<std::string> aliases;  // lower case
        uint32 mapId;
        uint32 level;                      // bot level: 60 vanilla, 70 TBC, 80 WotLK
        std::string reason;                // why this lineup, shown to the player
        std::map<uint32, Quota> quotas;    // by raid size

        // Every 40-man raid is a vanilla one, including the 40-man Onyxia and Naxxramas
        // that share a map with their level 80 versions.
        uint32 LevelFor(uint32 size) const { return size == 40 ? 60 : level; }
    };
    std::vector<EncounterLineup> encounters;

    EncounterLineup const* FindEncounter(std::string const& alias) const {
        for (EncounterLineup const& e : encounters)
            if (std::find(e.aliases.begin(), e.aliases.end(), alias) != e.aliases.end())
                return &e;
        return nullptr;
    }

    EncounterLineup const* FindEncounter(uint32 mapId) const {
        for (EncounterLineup const& e : encounters)
            if (e.mapId == mapId)
                return &e;
        return nullptr;
    }

    static BotRaidConfigData* instance() {
        static BotRaidConfigData instance;
        return &instance;
    }

    float LoadAndValidateFloat(const std::string& key, float def, float minVal, float maxVal) {
        float val = sConfigMgr->GetOption<float>(key, def);
        if (val < minVal || val > maxVal) {
            LOG_ERROR("server.loading", "[OptimalBotRaid] CONFIG ERROR: '{}' value ({}) is out of bounds [{} to {}]. Reverting to safe default ({}).", 
                      key, val, minVal, maxVal, def);
            return def;
        }
        return val;
    }

    int32 LoadAndValidateInt(const std::string& key, int32 def, int32 minVal, int32 maxVal) {
        int32 val = sConfigMgr->GetOption<int32>(key, def);
        if (val < minVal || val > maxVal) {
            LOG_ERROR("server.loading", "[OptimalBotRaid] CONFIG ERROR: '{}' value ({}) is out of bounds [{} to {}]. Reverting to safe default ({}).", 
                      key, val, minVal, maxVal, def);
            return def;
        }
        return val;
    }

    void Load() {
        weightGS          = LoadAndValidateFloat("OptimalBotRaid.Algo.Weight.GS", 0.5f, 0.0f, 10.0f);
        weightBuff        = LoadAndValidateFloat("OptimalBotRaid.Algo.Weight.Buff", 0.5f, 0.0f, 10.0f);
        buffBase          = LoadAndValidateFloat("OptimalBotRaid.Algo.Bonus.UniqueBuff", 0.2f, 0.0f, 10.0f);
        buffBloodlust     = LoadAndValidateFloat("OptimalBotRaid.Algo.Bonus.Bloodlust", 0.4f, 0.0f, 10.0f);
        buffReplenishment = LoadAndValidateFloat("OptimalBotRaid.Algo.Bonus.Replenishment", 0.2f, 0.0f, 10.0f);

        maxLevelAbovePlayer = LoadAndValidateInt("OptimalBotRaid.MaxLevelAbovePlayer", 2, 0, 80);
        sameLevelAtCaps     = sConfigMgr->GetOption<bool>("OptimalBotRaid.SameLevelAtCaps", true);
        raidBotLevel        = sConfigMgr->GetOption<bool>("OptimalBotRaid.RaidBotLevel", true);
        sortGroups          = sConfigMgr->GetOption<bool>("OptimalBotRaid.SortGroups", true);
        bearTanksInRaids    = sConfigMgr->GetOption<bool>("OptimalBotRaid.BearTanksInRaids", false);
        commandCooldown     = LoadAndValidateInt("OptimalBotRaid.CommandCooldown", 30, 0, 3600);

        const char* clsMap[12] = {"", "Warrior", "Paladin", "Hunter", "Rogue", "Priest", "DK", "Shaman", "Mage", "Warlock", "", "Druid"};
        float defMult[12][3] = {
            {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.2f}, {1.5f, 1.5f, 1.0f}, {1.1f, 1.2f, 1.2f}, 
            {0.8f, 1.5f, 0.8f}, {0.6f, 1.0f, 1.2f}, {1.2f, 1.5f, 0.6f}, {1.2f, 1.0f, 1.5f}, 
            {1.5f, 1.1f, 1.1f}, {1.0f, 1.0f, 1.5f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.5f, 1.0f} 
        };

        for (uint8 c = 1; c <= 11; ++c) {
            if (c == 10) continue;
            for (uint8 t = 0; t < 3; ++t) {
                std::string key = "OptimalBotRaid.AI." + std::string(clsMap[c]) + "." + std::to_string(t);
                aiMult[c][t] = LoadAndValidateFloat(key, defMult[c][t], 0.1f, 10.0f);
            }
        }

        auto LoadQuotaVal = [&](const std::string& prefix, Quota def, int limit) -> Quota {
            Quota q = def;
            q.tanks   = LoadAndValidateInt(prefix + ".Tanks", def.tanks, 0, limit);
            q.healers = LoadAndValidateInt(prefix + ".Healers", def.healers, 0, limit);
            q.melee   = LoadAndValidateInt(prefix + ".MeleeMax", def.melee, 0, limit);

            if ((q.tanks + q.healers + q.melee) > limit) {
                LOG_ERROR("server.loading", "[OptimalBotRaid] LOGIC ERROR: Configured slots for '{}' mathematically exceed {}. Resetting defaults.", prefix, limit);
                q.tanks = def.tanks; q.healers = def.healers; q.melee = def.melee;
            }

            // The class minimums share their role's slots. Only the lineups that need one list its
            // key, so a missing one isn't logged.
            int room[ROLE_UNKNOWN] = { q.tanks, q.healers, q.melee, limit - q.tanks - q.healers - q.melee };
            static char const* roleSlots[ROLE_UNKNOWN] = { "tank", "healer", "melee", "ranged" };
            for (ClassMinimum const& m : CLASS_MINIMUMS) {
                int& left = room[m.role];
                int32 val = sConfigMgr->GetOption<int32>(prefix + "." + m.configKey, def.*m.count, false);
                if (val < 0 || val > left) {
                    LOG_ERROR("server.loading", "[OptimalBotRaid] CONFIG ERROR: '{}.{}' value ({}) is out of bounds [0 to {} {} slots left]. Using {}.",
                              prefix, m.configKey, val, left, roleSlots[m.role], std::clamp(val, 0, left));
                    val = std::clamp(val, 0, left);
                }
                q.*m.count = val;
                left -= val;
            }
            for (ClassCap const& c : CLASS_CAPS) {
                int32 val = sConfigMgr->GetOption<int32>(prefix + "." + c.configKey, def.*c.count, false);
                if (val < -1 || val > limit) {
                    LOG_ERROR("server.loading", "[OptimalBotRaid] CONFIG ERROR: '{}.{}' value ({}) is out of bounds [-1 (no cap) to {}]. Using {}.",
                              prefix, c.configKey, val, limit, def.*c.count);
                    val = def.*c.count;
                }
                q.*c.count = val;
            }
            return q;
        };

        quotas[0] = LoadQuotaVal("OptimalBotRaid.Quota.5", { 1, 1, 1 }, 5);
        quotas[1] = LoadQuotaVal("OptimalBotRaid.Quota.10", { 2, 2, 2 }, 10);
        quotas[2] = LoadQuotaVal("OptimalBotRaid.Quota.15", { 2, 3, 3 }, 15);
        quotas[3] = LoadQuotaVal("OptimalBotRaid.Quota.20", { 2, 4, 5 }, 20);
        quotas[4] = LoadQuotaVal("OptimalBotRaid.Quota.25", { 2, 5, 6 }, 25);
        quotas[5] = LoadQuotaVal("OptimalBotRaid.Quota.40Vanilla", { 4, 10, 10 }, 40);
        quotas[6] = LoadQuotaVal("OptimalBotRaid.Quota.40WotLK", { 3, 8, 10 }, 40);


        // Defaults (tanks, healers, melee max; the vanilla 40-mans also set a class mix like a
        // vanilla raid's: priests and druids healing, rogues and warriors in melee, hunters,
        // warlocks and mages, at most 6 paladins and 6 shamans outside the tank slots); ranged
        // fills the rest.
        // Sources and reasoning are in the README. Shared maps (Onyxia, Naxxramas) key their
        // WotLK and 40-man versions by size; their level is 80, and LevelFor() makes the
        // 40-man versions 60.
        encounters = {
            // Vanilla
            { "Zul'Gurub", "ZulGurub", { "zg", "zulgurub" }, MAP_ZUL_GURUB, 60,
              "Hakkar mind-controls tanks and Thekal splits three ways; Venoxis and Arlokk punish melee",
              { { 20, { 3, 6, 3 } } } },
            { "Ruins of Ahn'Qiraj", "RuinsOfAhnQiraj", { "aq20", "ruins" }, MAP_RUINS_OF_AHN_QIRAJ, 60,
              "extra tank for Kurinnaxx swaps and Rajaxx waves; Moam, Ayamiss and Ossirian favor ranged",
              { { 20, { 3, 5, 3 } } } },
            { "Onyxia", "Onyxia", { "onyxia", "ony" }, MAP_ONYXIAS_LAIR, 80,
              "only ranged can hit her in the air",
              { { 10, { 2, 2, 1 } }, { 25, { 3, 5, 3 } },
                { 40, { .tanks = 3, .healers = 10, .melee = 4, .hunters = 3, .warlocks = 3, .mages = 5,
                        .priests = 4, .druidHealers = 2, .rogues = 2, .warriors = 2,
                        .paladinsMax = 6, .shamansMax = 6 } } } },
            { "Molten Core", "MoltenCore", { "mc", "moltencore", "molten" }, MAP_MOLTEN_CORE, 60,
              "extra tanks for Garr, Golemagg and Majordomo; Ragnaros knocks melee into the lava; hunters for Magmadar's Frenzy",
              { { 40, { .tanks = 5, .healers = 11, .melee = 8, .hunters = 3, .warlocks = 3, .mages = 5,
                        .priests = 4, .druidHealers = 2, .rogues = 3, .warriors = 3,
                        .paladinsMax = 6, .shamansMax = 6 } } } },
            { "Blackwing Lair", "BlackwingLair", { "bwl", "blackwinglair" }, MAP_BLACKWING_LAIR, 60,
              "five tanks for Vaelastrasz and the drakes; Vael and Nefarian need heavy healing; hunters for Flamegor's and Chromaggus' Frenzy; rogues and warriors because Chromaggus shrugs off most spells",
              { { 40, { .tanks = 5, .healers = 13, .melee = 7, .hunters = 3, .warlocks = 3, .mages = 5,
                        .priests = 5, .druidHealers = 2, .rogues = 3, .warriors = 3,
                        .paladinsMax = 6, .shamansMax = 6 } } } },
            { "Temple of Ahn'Qiraj", "TempleOfAhnQiraj", { "aq40", "temple" }, MAP_AHN_QIRAJ_TEMPLE, 60,
              "tanks for the Bug Trio, Sartura and Fankriss adds; Ouro and Huhuran punish melee; hunters for Huhuran's Frenzy",
              { { 40, { .tanks = 5, .healers = 12, .melee = 7, .hunters = 3, .warlocks = 3, .mages = 5,
                        .priests = 5, .druidHealers = 2, .rogues = 3, .warriors = 3,
                        .paladinsMax = 6, .shamansMax = 6 } } } },
            { "Naxxramas", "Naxxramas", { "naxx", "naxxramas" }, MAP_NAXXRAMAS, 80,
              "extra tanks for Patchwerk, Thaddius and the Horsemen (two each in the 40-man); Heigan and Sapphiron favor ranged",
              { { 10, { 2, 2, 2 } }, { 25, { 3, 5, 5 } },
                { 40, { .tanks = 8, .healers = 12, .melee = 6, .hunters = 2, .warlocks = 3, .mages = 5,
                        .priests = 5, .druidHealers = 2, .rogues = 3, .warriors = 2,
                        .paladinsMax = 6, .shamansMax = 6 } } } },
            // The Burning Crusade
            { "Karazhan", "Karazhan", { "kara", "karazhan" }, MAP_KARAZHAN, 70,
              "third healer for Prince and Nightbane; Aran and Prince punish melee",
              { { 10, { 2, 3, 2 } } } },
            { "Zul'Aman", "ZulAman", { "za", "zulaman" }, MAP_ZUL_AMAN, 70,
              "Nalorakk tank swap and Halazzi split; Akil'zon and Zul'jin's Whirlwind punish melee",
              { { 10, { 2, 3, 2 } } } },
            { "Gruul's Lair", "GruulsLair", { "gruul", "gruulslair" }, MAP_GRUULS_LAIR, 70,
              "three tanks for Maulgar's council; Whirlwind and Shatter punish melee",
              { { 25, { 3, 7, 4 } } } },
            { "Magtheridon's Lair", "MagtheridonsLair", { "mag", "magtheridon" }, MAP_MAGTHERIDONS_LAIR, 70,
              "three tanks to split the five Channelers, each with its own healer",
              { { 25, { 3, 7, 5 } } } },
            { "Serpentshrine Cavern", "SerpentshrineCavern", { "ssc", "serpentshrine" }, MAP_COILFANG_SERPENTSHRINE_CAVERN, 70,
              "four tanks for Karathress and his guards; Lurker and Leotheras punish melee",
              { { 25, { 4, 7, 4 } } } },
            { "Tempest Keep", "TempestKeep", { "tk", "tempestkeep" }, MAP_TEMPEST_KEEP, 70,
              "tanks for Al'ar and Kael'thas's advisors; Flamestrike punishes stacked melee",
              { { 25, { 3, 7, 5 } } } },
            { "Hyjal Summit", "HyjalSummit", { "hyjal", "mh" }, MAP_THE_BATTLE_FOR_MOUNT_HYJAL, 70,
              "third tank for trash waves, Infernals and Doomguards; Archimonde punishes melee",
              { { 25, { 3, 7, 5 } } } },
            { "Black Temple", "BlackTemple", { "bt", "blacktemple" }, MAP_BLACK_TEMPLE, 70,
              "three tanks for Bloodboil, Shahraz, the Council and Illidan's Flames; Illidan punishes melee",
              { { 25, { 3, 7, 4 } } } },
            { "Sunwell Plateau", "SunwellPlateau", { "swp", "sunwell" }, MAP_THE_SUNWELL, 70,
              "hardest healing tier; M'uru needs three tanks and Darkness pushes melee out",
              { { 25, { 3, 8, 4 } } } },
            // Wrath of the Lich King (Naxxramas and Onyxia are above)
            { "Obsidian Sanctum", "ObsidianSanctum", { "os", "obsidian", "sartharion" }, MAP_THE_OBSIDIAN_SANCTUM, 80,
              "fire walls and lava waves sweep the melee; the drakes need an off-tank",
              { { 10, { 2, 2, 1 } }, { 25, { 2, 5, 5 } } } },
            { "Eye of Eternity", "EyeOfEternity", { "eoe", "malygos" }, MAP_THE_EYE_OF_ETERNITY, 80,
              "ranged kill the Scions in phase 2 while tanks hold the Nexus Lords",
              { { 10, { 2, 2, 2 } }, { 25, { 2, 5, 5 } } } },
            { "Vault of Archavon", "VaultOfArchavon", { "voa", "vault", "archavon" }, MAP_VAULT_OF_ARCHAVON, 80,
              "Emalon's Lightning Nova punishes melee; Koralon and Toravon need tank swaps",
              { { 10, { 2, 2, 2 } }, { 25, { 2, 5, 5 } } } },
            { "Ulduar", "Ulduar", { "uld", "ulduar" }, MAP_ULDUAR, 80,
              "Iron Council needs extra tanks; Mimiron, XT and Vezax punish melee; heavy raid damage",
              { { 10, { 2, 3, 1 } }, { 25, { 3, 6, 4 } } } },
            { "Trial of the Crusader", "TrialOfTheCrusader", { "toc", "totc", "crusader" }, MAP_TRIAL_OF_THE_CRUSADER, 80,
              "Anub'arak's Burrowers need off-tanks; Champions and Leeching Swarm burst the raid",
              { { 10, { 2, 2, 2 } }, { 25, { 3, 6, 4 } } } },
            { "Icecrown Citadel", "IcecrownCitadel", { "icc", "icecrown" }, MAP_ICECROWN_CITADEL, 80,
              "Sindragosa, Marrowgar and the Lich King punish melee; Valithria and Putricide need healing",
              { { 10, { 2, 3, 1 } }, { 25, { 2, 6, 5 } } } },
            { "Ruby Sanctum", "RubySanctum", { "rs", "ruby", "halion" }, MAP_THE_RUBY_SANCTUM, 80,
              "Halion splits the raid: melee in the Twilight realm, ranged in the Physical realm",
              { { 10, { 2, 3, 1 } }, { 25, { 2, 6, 6 } } } },
        };

        for (EncounterLineup& e : encounters)
            for (auto& [size, quota] : e.quotas)
                quota = LoadQuotaVal("OptimalBotRaid.Quota." + e.configKey + "." + std::to_string(size), quota, (int)size);
    }
};

class OptimalBotRaidConfigScript : public WorldScript {
public:
    OptimalBotRaidConfigScript() : WorldScript("OptimalBotRaidConfigScript") {}
    void OnAfterConfigLoad(bool /*reload*/) override {
        BotRaidConfigData::instance()->Load();
    }
};

// Keeps s_draftedBots to bots still in a group: a bot that leaves, or a leader who leaves,
// drops out of it, and a disband clears the whole group. Battleground and battlefield groups
// are skipped: players sit in those next to their own group, which they still lead.
class OptimalBotRaidGroupScript : public GroupScript {
public:
    OptimalBotRaidGroupScript() : GroupScript("OptimalBotRaidGroupScript", { GROUPHOOK_ON_REMOVE_MEMBER, GROUPHOOK_ON_DISBAND }) {}

    void OnRemoveMember(Group* group, ObjectGuid guid, RemoveMethod /*method*/, ObjectGuid /*kicker*/, char const* /*reason*/) override {
        if (group->isBGGroup() || group->isBFGroup())
            return;
        Forget({ guid });
    }

    void OnDisband(Group* group) override {
        if (group->isBGGroup() || group->isBFGroup())
            return;
        std::vector<ObjectGuid> members;
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
            members.push_back(slot.guid);
        Forget(members);
    }

private:
    static void Forget(std::vector<ObjectGuid> const& guids) {
        std::lock_guard<std::mutex> lock(s_draftedBotsLock);
        for (ObjectGuid const& guid : guids)
            s_draftedBots.erase(guid);
        for (auto itr = s_draftedBots.begin(); itr != s_draftedBots.end();) {
            for (ObjectGuid const& guid : guids)
                itr->second.erase(guid);
            itr = itr->second.empty() ? s_draftedBots.erase(itr) : std::next(itr);
        }
    }
};

// ==============================================================================
// CORE ALGORITHM
// ==============================================================================

const char* GetRoleName(BotRole r) {
    switch(r) { case ROLE_TANK: return "TANK"; case ROLE_HEALER: return "HEALER"; case ROLE_MELEE: return "MELEE"; case ROLE_RANGED: return "RANGED"; default: return "UNKNOWN"; }
}

enum WotlkBuffs : uint32 {
    BUFF_BLOODLUST      = 1 << 0, BUFF_REPLENISHMENT  = 1 << 1,
    BUFF_10_PCT_STATS   = 1 << 2, BUFF_5_PCT_STATS    = 1 << 3,
    BUFF_SPELL_POWER    = 1 << 4, BUFF_MELEE_HASTE    = 1 << 5,
    BUFF_SPELL_HASTE    = 1 << 6, BUFF_10_PCT_AP      = 1 << 7,
    BUFF_ARMOR_PEN      = 1 << 8, BUFF_13_MAGIC_DMG   = 1 << 9,
    BUFF_MAX_HP         = 1 << 10, BUFF_ARCANE_INT    = 1 << 11,
    BUFF_3_RAID_DMG     = 1 << 12, BUFF_CRIT          = 1 << 13 
};

struct BotCandidate {
    Player* bot;
    uint32 level;
    BotRole role;
    float gearScore;
    float aiCompetency;
    uint32 providedBuffs;
};

class cs_optimal_bot_raid : public CommandScript
{
public:
    cs_optimal_bot_raid() : CommandScript("cs_optimal_bot_raid") { }

    ChatCommandTable GetCommands() const override {
        static ChatCommandTable botRaidTable = {
            { "finder",    HandleFinder,     SEC_PLAYER, Console::No },
            { "assemble",  HandleAssemble,   SEC_PLAYER, Console::No },
            { "dismiss",   HandleDismiss,    SEC_PLAYER, Console::No },
            { "debug",     HandleDebug,      SEC_GAMEMASTER, Console::No },
            { "telemetry", HandleTelemetry,  SEC_GAMEMASTER, Console::No },
            { "raids",     HandleRaids,      SEC_PLAYER, Console::No },
            { "sort",      HandleSort,       SEC_PLAYER, Console::No },
            { "unbind",    HandleUnbind,     SEC_PLAYER, Console::No },
            { "version",   HandleVersion,    SEC_PLAYER, Console::No }
        };
        static ChatCommandTable commandTable = { { "botraid", botRaidTable } };
        return commandTable;
    }

    #include "RaidFinderCommands.inc"

    // Lists the raid lineups and what to type for each.
    static bool HandleRaids(ChatHandler* handler)
    {
        handler->SendSysMessage("Raid lineups (bot level, tanks/healers/melee/ranged, class mix). Used inside the raid, or with .botraid assemble <raid> [size]:");
        for (auto const& e : BotRaidConfigData::instance()->encounters) {
            std::string lineups;
            for (auto const& [size, q] : e.quotas) {
                std::string mix = BotRaidConfigData::DescribeClassMix(q);
                lineups += (lineups.empty() ? "" : ", ") + std::to_string(size) + "-man L" + std::to_string(e.LevelFor(size)) + " " + std::to_string(q.tanks) + "/" +
                    std::to_string(q.healers) + "/" + std::to_string(q.melee) + "/" +
                    std::to_string((int)size - q.tanks - q.healers - q.melee) +
                    (mix.empty() ? "" : " (" + mix + ")");
            }
            handler->PSendSysMessage("  {} ({}): {}", e.name, e.aliases.front(), lineups);
        }
        return true;
    }

    static bool HandleVersion(ChatHandler* handler)
    {
        handler->SendSysMessage("Optimal Bot Raid Module");
        handler->SendSysMessage("Compiled On: |cff00ff00" __DATE__ " at " __TIME__ "|r");
        return true;
    }

    static bool HandleTelemetry(ChatHandler* handler, Optional<std::string> optArg)
    {
        if (optArg) {
            std::string argStr = *optArg;
            std::transform(argStr.begin(), argStr.end(), argStr.begin(), ::tolower);

            if (argStr == "on") {
                s_telemetryEnabled = true;
                handler->SendSysMessage("BotRaid Validation Telemetry: |cff00ff00ON|r. Air-gapped logs will capture algorithmic evaluation and full lifecycle bot states.");
                return true;
            } else if (argStr == "off") {
                s_telemetryEnabled = false;
                handler->SendSysMessage("BotRaid Validation Telemetry: |cffff0000OFF|r.");
                return true;
            }
        }
        
        handler->PSendSysMessage("BotRaid Validation Telemetry is currently: {}", s_telemetryEnabled ? "|cff00ff00ON|r" : "|cffff0000OFF|r");
        handler->SendSysMessage("Syntax: .botraid telemetry <on|off>");
        return true;
    }

    static bool HandleDebug(ChatHandler* handler)
    {
        handler->SendSysMessage("|cffff0000The .botraid debug command is DEPRECATED.|r");
        handler->SendSysMessage("Please use |cff00ff00.botraid telemetry on|r to capture comprehensive air-gapped state logs during assembly and dismissal.");
        return true;
    }

    static void AppendBotStateTelemetry(std::ostringstream& teleLog, Player* bot) {
        teleLog << "    State Profile: " << bot->GetName() << " (GUID: " << bot->GetGUID().ToString() << ")\n";
        
        if (MotionMaster* mm = bot->GetMotionMaster()) {
            teleLog << "    MotionMaster Type: " << mm->GetCurrentMovementGeneratorType() << "\n";
        }
        
        teleLog << "    Unit State: " << bot->GetUnitState() 
                << " | Combat State: " << (bot->IsInCombat() ? "TRUE" : "FALSE") 
                << " | Grouped: " << (bot->GetGroup() ? "TRUE" : "FALSE") << "\n";

        if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot)) {
            teleLog << "    PlayerbotAI: ACTIVE\n"
                    << "    AI State: " << ai->GetState() << "\n";
            
            if (Player* master = ai->GetMaster()) {
                teleLog << "    AI Master: " << master->GetName() << " (GUID: " << master->GetGUID().ToString() << ")\n";
            } else {
                teleLog << "    AI Master: NONE\n";
            }

            auto dumpStrats = [&](BotState state, const char* stateName) {
                teleLog << "    Strategies (" << stateName << "): ";
                std::vector<std::string> strats = ai->GetStrategies(state);
                for (const auto& s : strats) teleLog << s << " ";
                teleLog << "\n";
            };

            dumpStrats(BOT_STATE_COMBAT, "COMBAT");
            dumpStrats(BOT_STATE_NON_COMBAT, "NON_COMBAT");
            dumpStrats(BOT_STATE_DEAD, "DEAD");
        } else {
            teleLog << "    PlayerbotAI: NULL (Bot logic engine missing)\n";
        }
    }

    static uint8 GetBotSpec(Player* bot, uint8 cls) {
        auto HasSpell = [&](std::initializer_list<uint32> spells) {
            for (uint32 id : spells) { if (bot->HasSpell(id)) return true; }
            return false;
        };
        switch (cls) {
            case CLASS_DEATH_KNIGHT: if (HasSpell({49028, 55233})) return 0; if (HasSpell({49143, 51411})) return 1; if (HasSpell({49206, 55090})) return 2; break;
            case CLASS_PALADIN: if (HasSpell({53563, 20473})) return 0; if (HasSpell({53595, 31935})) return 1; if (HasSpell({53385, 20066})) return 2; break;
            case CLASS_MAGE: if (HasSpell({44425, 12042})) return 0; if (HasSpell({44457, 55360})) return 1; if (HasSpell({44572, 11426})) return 2; break;
            case CLASS_SHAMAN: if (HasSpell({51490, 59159})) return 0; if (HasSpell({51533, 17364})) return 1; if (HasSpell({61295, 16190})) return 2; break;
            case CLASS_WARLOCK: if (HasSpell({48181, 59164})) return 0; if (HasSpell({47241, 19028})) return 1; if (HasSpell({50796, 59172})) return 2; break;
            case CLASS_DRUID: if (HasSpell({48505, 53201})) return 0; if (HasSpell({50334, 17007})) return 1; if (HasSpell({48438, 53251})) return 2; break;
            case CLASS_PRIEST: if (HasSpell({47540, 53007})) return 0; if (HasSpell({47788, 34861})) return 1; if (HasSpell({47585, 15473})) return 2; break;
            case CLASS_HUNTER: if (HasSpell({53270, 19574})) return 0; if (HasSpell({53209, 19506})) return 1; if (HasSpell({53301, 60053})) return 2; break;
            case CLASS_ROGUE: if (HasSpell({51662, 14983})) return 0; if (HasSpell({51690, 13750})) return 1; if (HasSpell({51713, 14183})) return 2; break;
            case CLASS_WARRIOR: if (HasSpell({46924, 12294})) return 0; if (HasSpell({46917, 23881})) return 1; if (HasSpell({46968, 23922})) return 2; break;
        }
        
        if (PlayerbotsMgr::instance().GetPlayerbotAI(bot)) {
            if (PlayerbotAI::IsTank(bot)) return (cls == CLASS_WARRIOR ? 2 : ((cls == CLASS_PALADIN || cls == CLASS_DRUID) ? 1 : 0));
            if (PlayerbotAI::IsHeal(bot)) return (cls == CLASS_PALADIN ? 0 : (cls == CLASS_PRIEST ? 1 : 2));
            if (PlayerbotAI::IsRanged(bot) && cls == CLASS_SHAMAN) return 0;
            if (!PlayerbotAI::IsRanged(bot) && cls == CLASS_SHAMAN) return 1;
        }
        return 1; 
    }

    static void MapBotProfile(Player* bot, uint8 tree, BotRole& role, float& aiMult, uint32& buffs) {
        uint8 cls = bot->getClass(); 
        uint32 lvl = bot->GetLevel();
        buffs = 0;

        if (cls >= 1 && cls <= 11 && cls != 10 && tree < 3) {
            aiMult = BotRaidConfigData::instance()->aiMult[cls][tree];
        } else {
            aiMult = 1.0f;
        }

        switch (cls) {
            case CLASS_DEATH_KNIGHT:
                if (tree == 0) { role = ROLE_TANK; if (lvl >= 50) buffs |= BUFF_10_PCT_STATS; }
                else if (tree == 1) { role = ROLE_MELEE; if (lvl >= 40) buffs |= BUFF_MELEE_HASTE; } 
                else { role = ROLE_MELEE; if (lvl >= 50) buffs |= BUFF_13_MAGIC_DMG; } 
                break;
            case CLASS_PALADIN:
                if (lvl >= 16) buffs |= BUFF_10_PCT_STATS; 
                if (tree == 0) { role = ROLE_HEALER; } 
                else if (tree == 1) { role = ROLE_TANK; if (lvl >= 40) buffs |= BUFF_3_RAID_DMG; } 
                else { role = ROLE_MELEE; if (lvl >= 50) buffs |= BUFF_REPLENISHMENT; if (lvl >= 40) buffs |= BUFF_3_RAID_DMG; }
                break;
            case CLASS_MAGE:
                if (lvl >= 1) buffs |= BUFF_ARCANE_INT; role = ROLE_RANGED;
                if (tree == 0) { if (lvl >= 40) buffs |= BUFF_3_RAID_DMG; } 
                break;
            case CLASS_SHAMAN:
                if (lvl >= 70) buffs |= BUFF_BLOODLUST; 
                if (tree == 0) { role = ROLE_RANGED; if (lvl >= 50) buffs |= BUFF_SPELL_POWER; if (lvl >= 40) buffs |= BUFF_CRIT; }
                else if (tree == 1) { role = ROLE_MELEE; if (lvl >= 40) buffs |= BUFF_10_PCT_AP; if (lvl >= 30) buffs |= BUFF_MELEE_HASTE; }
                else { role = ROLE_HEALER; if (lvl >= 40) buffs |= BUFF_SPELL_HASTE; }
                break;
            case CLASS_WARLOCK:
                role = ROLE_RANGED; if (lvl >= 4) buffs |= BUFF_MAX_HP; 
                if (tree == 0) { if (lvl >= 50) buffs |= BUFF_13_MAGIC_DMG; }
                else if (tree == 1) { if (lvl >= 50) buffs |= BUFF_SPELL_POWER; } 
                else { if (lvl >= 50) buffs |= BUFF_REPLENISHMENT; } 
                break;
            case CLASS_DRUID:
                if (lvl >= 1) buffs |= BUFF_5_PCT_STATS;
                if (tree == 0) { role = ROLE_RANGED; if (lvl >= 50) buffs |= BUFF_13_MAGIC_DMG; if (lvl >= 40) buffs |= BUFF_SPELL_HASTE; }
                else if (tree == 1) {
                    bool bearBot = bot->HasAura(SPELL_DRUID_THICK_HIDE) && PlayerbotsMgr::instance().GetPlayerbotAI(bot);
                    role = bearBot ? ROLE_TANK : ROLE_MELEE; if (lvl >= 40) buffs |= BUFF_CRIT;
                }
                else { role = ROLE_HEALER; }
                break;
            case CLASS_PRIEST:
                if (lvl >= 1) buffs |= BUFF_MAX_HP; 
                if (tree == 0) { role = ROLE_HEALER; } 
                else if (tree == 1) { role = ROLE_HEALER; }
                else { role = ROLE_RANGED; if (lvl >= 50) buffs |= BUFF_REPLENISHMENT; }
                break;
            case CLASS_HUNTER:
                role = ROLE_RANGED;
                if (tree == 0) { if (lvl >= 50) buffs |= BUFF_3_RAID_DMG; }
                else if (tree == 1) { if (lvl >= 40) buffs |= BUFF_10_PCT_AP; }
                else { if (lvl >= 50) buffs |= BUFF_REPLENISHMENT; }
                break;
            case CLASS_ROGUE:
                role = ROLE_MELEE; if (lvl >= 14) buffs |= BUFF_ARMOR_PEN; 
                break;
            case CLASS_WARRIOR:
                if (lvl >= 10) buffs |= BUFF_ARMOR_PEN | BUFF_MAX_HP; 
                if (tree == 2) { role = ROLE_TANK; }
                else { role = ROLE_MELEE; if (lvl >= 40) buffs |= BUFF_CRIT; }
                break;
            default: role = ROLE_UNKNOWN; break;
        }
    }

    // True, after telling the player how long is left, while their last use of the command is
    // under OptimalBotRaid.CommandCooldown seconds ago. GMs never wait.
    static bool OnCooldown(ChatHandler* handler, std::unordered_map<ObjectGuid, Seconds> const& lastUse, char const* command)
    {
        uint32 cooldown = BotRaidConfigData::instance()->commandCooldown;
        if (!cooldown || handler->GetSession()->GetSecurity() >= SEC_GAMEMASTER)
            return false;

        auto itr = lastUse.find(handler->GetSession()->GetPlayer()->GetGUID());
        if (itr == lastUse.end())
            return false;

        Seconds left = itr->second + Seconds(cooldown) - GameTime::GetGameTime();
        if (left <= 0s)
            return false;

        handler->PSendSysMessage("You can use .botraid {} again in {} second{}.", command, left.count(), left.count() == 1 ? "" : "s");
        return true;
    }

    static void StartCooldown(Player* player, std::unordered_map<ObjectGuid, Seconds>& lastUse)
    {
        Seconds now = GameTime::GetGameTime();
        Seconds cooldown(BotRaidConfigData::instance()->commandCooldown);
        // Only players still cooling down stay in the map.
        std::erase_if(lastUse, [&](auto const& entry) { return entry.second + cooldown <= now; });
        lastUse[player->GetGUID()] = now;
    }

    static bool HandleAssemble(ChatHandler* handler, std::string arg1, Optional<uint32> optSize)
    {
        // Raid lineup (.botraid assemble onyxia 40, .botraid assemble mc), for assembling
        // outside the instance.
        std::string lower = arg1;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (auto const* encounter = BotRaidConfigData::instance()->FindEncounter(lower)) {
            if (optSize)
                return ExecuteAssemble(handler, 0, 0, false, *optSize, encounter);
            if (encounter->quotas.size() == 1)
                return ExecuteAssemble(handler, 0, 0, false, encounter->quotas.begin()->first, encounter);

            std::string sizes;
            for (auto const& [size, quota] : encounter->quotas)
                sizes += (sizes.empty() ? "" : ", ") + std::to_string(size);
            handler->PSendSysMessage("Give a raid size for {}: .botraid assemble {} <{}>", encounter->name, lower, sizes);
            return true;
        }

        // Case 1: The user passed two arguments (.botraid assemble 60-67 40)
        if (optSize) {
            uint32 reqMin = 0;
            uint32 reqMax = 0;
            uint32 size = *optSize;
            
            size_t dash = arg1.find('-');
            if (dash != std::string::npos) {
                try {
                    reqMin = std::stoul(arg1.substr(0, dash));
                    reqMax = std::stoul(arg1.substr(dash + 1));
                    return ExecuteAssemble(handler, reqMin, reqMax, true, size);
                } catch (...) {
                    handler->SendSysMessage("Invalid range syntax. Please provide valid numbers (e.g., 60-67).");
                    return true;
                }
            }
            
            handler->SendSysMessage("Invalid syntax. Example: .botraid assemble 60-67 40");
            return true;
        }

        // Case 2: The user passed one argument (.botraid assemble 40)
        try {
            uint32 size = std::stoul(arg1);
            return ExecuteAssemble(handler, 0, 0, false, size);
        } catch (...) {
            handler->SendSysMessage("Invalid syntax. Example: .botraid assemble 40, .botraid assemble 60-67 40 or .botraid assemble mc (see .botraid raids)");
            return true;
        }
    }

    static bool ExecuteAssemble(ChatHandler* handler, uint32 reqMin, uint32 reqMax, bool hasCustomRange, uint32 size,
        BotRaidConfigData::EncounterLineup const* encounter = nullptr)
    {
        Player* player = handler->GetSession()->GetPlayer();

        if (OnCooldown(handler, s_lastAssemble, "assemble"))
            return true;
        
        if (!player->IsAlive()) {
            handler->SendSysMessage("You cannot draft mercenaries while dead.");
            return true;
        }

        if (size != 5 && size != 10 && size != 15 && size != 20 && size != 25 && size != 40) {
            handler->SendSysMessage("Invalid size. Supported sizes: 5, 10, 15, 20, 25, 40.");
            return true;
        }

        BotRaidConfigData* cfg = BotRaidConfigData::instance();

        uint32 pLevel = player->GetLevel();
        // Players may not draft bots far above their own level; GMs are exempt.
        bool isGM = handler->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
        uint32 bracketMax = (pLevel <= 60) ? 60 : ((pLevel <= 70) ? 70 : 80);
        // Never past the end of the player's expansion bracket either (a 59 can't pull 61s).
        uint32 levelCap = isGM ? STRONG_MAX_LEVEL : std::min<uint32>(pLevel + cfg->maxLevelAbovePlayer, bracketMax);
        // At 60, 70 and 80 players only draft bots of exactly their level.
        bool atLevelCap = !isGM && cfg->sameLevelAtCaps && (pLevel == 60 || pLevel == 70 || pLevel == 80);

        // Raid lineup when asked for, or when assembling inside that raid.
        bool const askedForEncounter = encounter != nullptr;
        if (!encounter)
            encounter = cfg->FindEncounter(player->GetMapId());

        // Raids only get bots of the raid's level: 60 classic, 70 TBC, 80 WotLK. This holds
        // even when the raid has no lineup for the asked size.
        uint32 raidLevel = (encounter && cfg->raidBotLevel) ? encounter->LevelFor(size) : 0;
        std::string raidName = encounter ? encounter->name : "";

        if (raidLevel) {
            if (raidLevel > levelCap) {
                handler->PSendSysMessage("{} needs level {} bots, and you can only draft bots up to level {}.", raidName, raidLevel, levelCap);
                return true;
            }
            if (hasCustomRange && (reqMin != raidLevel || reqMax != raidLevel))
                handler->PSendSysMessage("{} only takes level {} bots; ignoring the level range.", raidName, raidLevel);
            reqMin = reqMax = raidLevel;
            atLevelCap = true; // exact level, no lower-level fill-ins
        } else if (atLevelCap) {
            if (hasCustomRange && (reqMin != pLevel || reqMax != pLevel))
                handler->PSendSysMessage("At level {} you can only draft level {} bots.", pLevel, pLevel);
            reqMin = reqMax = pLevel;
        } else if (!hasCustomRange) {
            reqMax = std::min<uint32>(levelCap, bracketMax);
            reqMin = (pLevel > 4) ? pLevel - 4 : 1;
        } else {
            if (reqMin > reqMax) std::swap(reqMin, reqMax);
            if (reqMin < 1) reqMin = 1;

            if (reqMin > levelCap) {
                handler->PSendSysMessage("You can only draft bots up to level {}.", levelCap);
                return true;
            }
            if (reqMax > levelCap) {
                handler->PSendSysMessage("Bots above level {} cannot be drafted. Using range {} - {}.", levelCap, reqMin, levelCap);
                reqMax = levelCap;
            }
        }

        Group* group = player->GetGroup();
        
        if (size == 5 && group && group->isRaidGroup()) {
            handler->SendSysMessage("You are currently in a Raid group. Please disband before assembling a 5-man party.");
            return true;
        }

        std::ostringstream teleLog;
        bool isTele = s_telemetryEnabled;
        
        if (isTele) {
            teleLog << "========================================================\n"
                    << "         OPTIMAL BOT RAID - ASSEMBLY TELEMETRY          \n"
                    << "========================================================\n"
                    << "Requested Size: " << size << "-man\n"
                    << "Target Level Range: " << reqMin << " - " << reqMax << (hasCustomRange ? " (Custom)\n" : " (Auto)\n")
                    << "Leader: " << player->GetName() << " (Lvl " << pLevel << ")\n\n"
                    << "--- CONFIGURATION WEIGHTS ---\n"
                    << "WeightGS: " << cfg->weightGS << " | WeightBuff: " << cfg->weightBuff << "\n"
                    << "BonusBase: " << cfg->buffBase << " | Bloodlust: " << cfg->buffBloodlust << " | Replenishment: " << cfg->buffReplenishment << "\n\n";
        }

        int qIdx = 0;
        if (size == 10) qIdx = 1;
        else if (size == 15) qIdx = 2;
        else if (size == 20) qIdx = 3;
        else if (size == 25) qIdx = 4;
        else if (size == 40) qIdx = ((raidLevel ? raidLevel : pLevel) <= 60) ? 5 : 6;

        BotRaidConfigData::Quota q = cfg->quotas[qIdx];

        BotRaidConfigData::Quota const* lineup = nullptr;
        if (encounter) {
            auto it = encounter->quotas.find(size);
            if (it != encounter->quotas.end())
                lineup = &it->second;
        }
        if (encounter && !lineup) {
            if (askedForEncounter)
                handler->PSendSysMessage("No {} lineup for {}-man; using the standard {}-man lineup.", encounter->name, size, size);
            encounter = nullptr;
        }
        if (encounter) {
            q = *lineup;
            int ranged = (int)size - q.tanks - q.healers - q.melee;
            std::string mix = BotRaidConfigData::DescribeClassMix(q);
            handler->PSendSysMessage("{} lineup: {} tanks, {} healers, {} melee, {} ranged{} ({}).",
                encounter->name, q.tanks, q.healers, q.melee, ranged,
                mix.empty() ? "" : ", " + mix, encounter->reason);
            if (isTele)
                teleLog << "Encounter Lineup: " << encounter->name << " " << size << "-man\n\n";
        }

        int reqTanks = std::min(q.tanks, (int)size);
        int reqHealers = std::min(q.healers, (int)size - reqTanks);
        int reqMelee = std::min(q.melee, (int)size - reqTanks - reqHealers);
        int reqRanged = size - reqTanks - reqHealers - reqMelee;
        int* reqOf[ROLE_UNKNOWN] = { &reqTanks, &reqHealers, &reqMelee, &reqRanged };
        // Class minimums still to draft, in CLASS_MINIMUMS order; part of their role's count.
        constexpr size_t MINIMUMS = std::size(BotRaidConfigData::CLASS_MINIMUMS);
        int reqClass[MINIMUMS];
        for (size_t i = 0; i < MINIMUMS; ++i)
            reqClass[i] = q.*BotRaidConfigData::CLASS_MINIMUMS[i].count;
        // Members of each class outside the tank slots, in the group and drafted, for the class caps.
        int classCount[MAX_CLASSES] = {};

        uint32 currentRaidBuffs = 0;
        uint32 currentMembers = 0;

        // Bears stay out of raids: warriors, paladins and death knights tank those.
        bool noBearTanks = size > 5 && !cfg->bearTanksInRaids;

        auto EvalPlayer = [&](Player* p) {
            currentMembers++;
            BotRole role; float comp; uint32 buffs;
            MapBotProfile(p, GetBotSpec(p, p->getClass()), role, comp, buffs);
            currentRaidBuffs |= buffs;

            // A bear bot that is already in the group doesn't use up one of the raid's tank slots.
            if (noBearTanks && role == ROLE_TANK && p->getClass() == CLASS_DRUID) {
                role = ROLE_MELEE;
                handler->PSendSysMessage("{} is a bear tank and isn't counted as one of this raid's tanks.", p->GetName());
            }
            
            if (isTele) {
                teleLog << "  Group Member: " << p->GetName() << " | Role: " << GetRoleName(role) 
                        << " | Buffs (Hex): 0x" << std::hex << buffs << std::dec << "\n";
            }

            if (role == ROLE_TANK) reqTanks--;
            else if (role == ROLE_HEALER) reqHealers--;
            else if (role == ROLE_MELEE) reqMelee--;
            else { role = ROLE_RANGED; reqRanged--; }

            for (size_t i = 0; i < MINIMUMS; ++i) {
                auto const& m = BotRaidConfigData::CLASS_MINIMUMS[i];
                if (p->getClass() == m.cls && role == m.role) reqClass[i]--;
            }
            if (role != ROLE_TANK && p->getClass() < MAX_CLASSES)
                classCount[p->getClass()]++;
        };

        if (group) {
            for (GroupReference* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next()) {
                if (Player* member = ref->GetSource()) EvalPlayer(member);
            }
        } else {
            EvalPlayer(player);
        }

        reqTanks = std::max(0, reqTanks); reqHealers = std::max(0, reqHealers);
        reqMelee = std::max(0, reqMelee); reqRanged = std::max(0, reqRanged);
        for (int& req : reqClass) req = std::max(0, req);
        
        int botsToDraft = reqTanks + reqHealers + reqMelee + reqRanged;
        int slotsAvailable = (int)size - (int)currentMembers;

        if (isTele) {
            teleLog << "\n--- TARGET QUOTAS & STATE ---\n"
                    << "Target Draft: " << botsToDraft << " bots (T:" << reqTanks << " H:" << reqHealers << " M:" << reqMelee << " R:" << reqRanged << ")\n"
                    << "Class Minimums:";
            for (size_t i = 0; i < MINIMUMS; ++i)
                teleLog << " " << BotRaidConfigData::CLASS_MINIMUMS[i].configKey << ":" << reqClass[i];
            teleLog << "\n"
                    << "Initial Matrix Buffs: 0x" << std::hex << currentRaidBuffs << std::dec << "\n\n";
        }

        if (slotsAvailable <= 0) {
            handler->SendSysMessage("Your group is already full or matches the requested size.");
            return true;
        }

        if (botsToDraft > slotsAvailable) {
            int excess = botsToDraft - slotsAvailable;
            while (excess > 0) {
                if (reqRanged > 0) { reqRanged--; excess--; }
                else if (reqMelee > 0) { reqMelee--; excess--; }
                else if (reqHealers > 0) { reqHealers--; excess--; }
                else if (reqTanks > 0) { reqTanks--; excess--; }
            }
            botsToDraft = slotsAvailable; 
        }

        // The class minimums come out of their role's slots; a group that already fills that role
        // with other classes has no room for them.
        int roleRoom[ROLE_UNKNOWN] = { reqTanks, reqHealers, reqMelee, reqRanged };
        for (size_t i = 0; i < MINIMUMS; ++i) {
            auto const& m = BotRaidConfigData::CLASS_MINIMUMS[i];
            int& room = roleRoom[m.role];
            if (reqClass[i] > room) {
                handler->PSendSysMessage("This lineup wants {} more {}, but your group only has room for {}.",
                    reqClass[i], reqClass[i] == 1 ? m.singular : m.plural, room);
                reqClass[i] = room;
            }
            room -= reqClass[i];
        }

        bool crossFaction = sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP);

        std::vector<ObjectGuid> potentialBots;
        {
            std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
            auto const& players = ObjectAccessor::GetPlayers();
            
            for (auto const& pair : players) {
                Player* bot = pair.second;
                if (!bot || bot == player || bot->GetGroup() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST)) continue;
                if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot)) continue;
                // Other-faction bots only when the core allows mixed groups (AllowTwoSide.Interaction.Group).
                if (!crossFaction && bot->GetTeamId() != player->GetTeamId()) continue;
                // Only random bots: never pull another player's alt bots.
                if (!sRandomPlayerbotMgr.IsRandomBot(bot)) continue;
                // Leave bots alone that are queued for LFG/BGs or sitting inside an instance.
                if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE || bot->InBattlegroundQueue()) continue;
                if (bot->GetMap()->Instanceable()) continue;
                
                uint32 bLevel = bot->GetLevel();
                if (bLevel > reqMax) continue;
                
                potentialBots.push_back(bot->GetGUID());
            }
        }

        std::vector<BotCandidate> allCandidates;
        for (ObjectGuid guid : potentialBots) {
            if (Player* bot = ObjectAccessor::FindConnectedPlayer(guid)) {
                if (bot->GetGroup() || !bot->IsAlive() || bot->IsInCombat()) continue;

                BotCandidate cand;
                cand.bot = bot;
                cand.level = bot->GetLevel();
                cand.gearScore = std::max(1.0f, (float)bot->GetAverageItemLevel()); 
                
                MapBotProfile(bot, GetBotSpec(bot, bot->getClass()), cand.role, cand.aiCompetency, cand.providedBuffs);
                if (noBearTanks && cand.role == ROLE_TANK && bot->getClass() == CLASS_DRUID) continue;
                if (cand.role != ROLE_UNKNOWN) allCandidates.push_back(cand);
            }
        }

        std::vector<BotCandidate> pool;
        uint32 currentMin = reqMin;
        bool relaxed = false;

        // Intelligent boundary relaxation - sequentially steps down to level 10 to salvage failing drafts
        while (true) {
            pool.clear();
            for (auto const& c : allCandidates) {
                if (c.level >= currentMin) {
                    pool.push_back(c);
                }
            }
            if (pool.size() >= (size_t)botsToDraft || currentMin <= 10 || atLevelCap) {
                break;
            }
            currentMin--;
            relaxed = true;
        }

        if (pool.size() < (size_t)botsToDraft) {
            if (reqMin == reqMax && pool.empty()) {
                if (raidLevel)
                    handler->PSendSysMessage("No idle level {} bots found for {}. Nobody was invited.", reqMin, raidName);
                else
                    handler->PSendSysMessage("No idle level {} bots found. Nobody was invited.", reqMin);
            } else if (reqMin == reqMax) {
                if (raidLevel)
                    handler->PSendSysMessage("Not enough idle level {} bots for {}: found {}, need {}. Nobody was invited.", reqMin, raidName, pool.size(), botsToDraft);
                else
                    handler->PSendSysMessage("Not enough idle level {} bots: found {}, need {}. Nobody was invited.", reqMin, pool.size(), botsToDraft);
            } else if (relaxed && currentMin < reqMin) {
                handler->PSendSysMessage("Not enough eligible idle bots found! Found {}. Needed {}. (Relaxed minimum level down to {})", pool.size(), botsToDraft, currentMin);
            } else {
                handler->PSendSysMessage("Not enough eligible idle bots found! Found {}. Needed {}.", pool.size(), botsToDraft);
            }
            return true;
        }

        if (relaxed && currentMin < reqMin) {
            handler->PSendSysMessage("Not enough bots in range ({} - {}). Relaxed minimum level down to {} to fulfill the draft.", reqMin, reqMax, currentMin);
            if (isTele) {
                teleLog << "--- RELAXATION EVENT ---\n"
                        << "Target min level " << reqMin << " did not yield enough candidates.\n"
                        << "Relaxed min level down to " << currentMin << " to fulfill draft.\n\n";
            }
        }

        float maxGS = 1.0f;
        for (auto const& c : pool) {
            if (c.gearScore > maxGS) maxGS = c.gearScore;
        }

        if (isTele) {
            teleLog << "--- CANDIDATE POOL (Size: " << pool.size() << ") ---\n"
                    << "Max GS of valid pool: " << maxGS << "\n";
            for (auto const& c : pool) {
                teleLog << "[" << c.bot->GetName() << "] Role: " << std::setw(6) << GetRoleName(c.role) 
                        << " | Lvl: " << std::setw(2) << c.level 
                        << " | Base GS: " << std::setw(4) << c.gearScore 
                        << " | AI_Mult: " << std::fixed << std::setprecision(1) << c.aiCompetency 
                        << " | Buff Mask: 0x" << std::hex << c.providedBuffs << std::dec << "\n";
            }
            teleLog << "\n--- SET COVER + KNAPSACK DRAFTING TRACE ---\n";
        }

        std::vector<Player*> draftedBots;
        
        // Whether the raid already has as many of this class outside the tank slots as the lineup
        // caps it at.
        auto AtCap = [&](uint8 cls) -> bool {
            for (auto const& c : BotRaidConfigData::CLASS_CAPS)
                if (c.cls == cls && q.*c.count >= 0 && cls < MAX_CLASSES && classCount[cls] >= q.*c.count)
                    return true;
            return false;
        };
        // Bots drafted past a class cap because nobody else could take the slot.
        int overCap[MAX_CLASSES] = {};

        // cls: only draft that class (0 = any class of the role). keepCaps: skip classes at their cap
        // (tank slots have no caps).
        auto DraftBestBot = [&](BotRole targetRole, uint8 cls = 0, bool keepCaps = true) -> bool {
            float bestScore = -1.0f; 
            int bestIndex = -1;
            float t_normGS = 0.0f, t_buffScore = 0.0f;
            int t_newBuffs = 0;
            
            if (isTele) teleLog << "Drafting Required Role: " << GetRoleName(targetRole) << (cls ? " (class " + std::to_string(cls) + ")" : "") << "\n";

            for (size_t i = 0; i < pool.size(); ++i) {
                if (pool[i].role != targetRole) continue;
                if (cls && pool[i].bot->getClass() != cls) continue;
                if (keepCaps && targetRole != ROLE_TANK && AtCap(pool[i].bot->getClass())) continue;

                uint32 newBuffs = (~currentRaidBuffs & pool[i].providedBuffs);
                int newBuffCount = 0; 
                uint32 mask = newBuffs;
                while (mask) { mask &= (mask - 1); newBuffCount++; }

                float normalizedGS = (pool[i].gearScore / maxGS) * pool[i].aiCompetency;
                float buffScore = (newBuffCount * cfg->buffBase);
                
                if (newBuffs & BUFF_BLOODLUST) buffScore += cfg->buffBloodlust; 
                if (newBuffs & BUFF_REPLENISHMENT) buffScore += cfg->buffReplenishment;

                float finalScore = (normalizedGS * cfg->weightGS) + (buffScore * cfg->weightBuff);

                if (isTele) {
                    teleLog << "  Eval: " << std::left << std::setw(12) << pool[i].bot->GetName() 
                            << " | nGS: " << std::fixed << std::setprecision(3) << normalizedGS 
                            << " | BuffSc: " << buffScore 
                            << " | Final: " << finalScore << "\n";
                }

                if (finalScore > bestScore) { 
                    bestScore = finalScore; 
                    bestIndex = i; 
                    if (isTele) { t_normGS = normalizedGS; t_buffScore = buffScore; t_newBuffs = newBuffCount; }
                }
            }

            if (bestIndex != -1) {
                if (isTele) {
                    teleLog << "  >>> SELECTED: " << pool[bestIndex].bot->GetName() 
                            << " | Score: " << bestScore << " (NormGS*W: " << (t_normGS * cfg->weightGS) 
                            << " + Buff*W: " << (t_buffScore * cfg->weightBuff) << ")\n"
                            << "      New Unique Buffs Added: " << t_newBuffs << "\n"
                            << "      Buff Matrix Trans.: 0x" << std::hex << currentRaidBuffs << " -> 0x" << (currentRaidBuffs | pool[bestIndex].providedBuffs) << std::dec << "\n\n";
                }

                uint8 draftedClass = pool[bestIndex].bot->getClass();
                if (targetRole != ROLE_TANK && draftedClass < MAX_CLASSES) {
                    if (AtCap(draftedClass)) overCap[draftedClass]++;
                    classCount[draftedClass]++;
                }
                draftedBots.push_back(pool[bestIndex].bot);
                currentRaidBuffs |= pool[bestIndex].providedBuffs; 
                pool.erase(pool.begin() + bestIndex);
                return true;
            }
            
            if (isTele) teleLog << "  >>> NO CANDIDATES FOUND FOR ROLE.\n\n";
            return false;
        };

        // A class under its cap first; one past it rather than leave the slot to another role.
        auto DraftRole = [&](BotRole targetRole, uint8 cls = 0) -> bool {
            return DraftBestBot(targetRole, cls, true) || (targetRole != ROLE_TANK && DraftBestBot(targetRole, cls, false));
        };

        // Each role takes its class minimums first, then any class for the rest of its slots.
        // Roles nobody could fill get filled with other roles below; tell the player.
        int missing[ROLE_UNKNOWN] = {};
        int missingClass[MINIMUMS] = {};
        for (int r = 0; r < ROLE_UNKNOWN; ++r) {
            BotRole const role = BotRole(r);
            for (size_t c = 0; c < MINIMUMS; ++c) {
                auto const& m = BotRaidConfigData::CLASS_MINIMUMS[c];
                if (m.role != role)
                    continue;
                for (int i = 0; i < reqClass[c]; i++) {
                    if (DraftRole(role, m.cls)) (*reqOf[r])--;
                    else missingClass[c]++;
                }
            }
            for (int i = 0; i < *reqOf[r]; i++) if (!DraftRole(role)) missing[r]++;
        }

        static char const* roleNames[ROLE_UNKNOWN] = { "tanks", "healers", "melee", "ranged" };
        for (int r = 0; r < ROLE_UNKNOWN; ++r)
            if (missing[r])
                handler->PSendSysMessage("Not enough {}{}: {} short, filled with other roles.", roleNames[r],
                    reqMin == reqMax ? " at level " + std::to_string(reqMin) : std::string(), missing[r]);
        for (size_t c = 0; c < MINIMUMS; ++c)
            if (missingClass[c]) {
                auto const& m = BotRaidConfigData::CLASS_MINIMUMS[c];
                handler->PSendSysMessage("Not enough {}{}: {} short, filled with other {}.", m.plural,
                    reqMin == reqMax ? " at level " + std::to_string(reqMin) : std::string(), missingClass[c], roleNames[m.role]);
            }

        // Any role under the caps before any past them.
        while ((int)draftedBots.size() < botsToDraft && !pool.empty()) {
            if (!DraftBestBot(ROLE_RANGED) && !DraftBestBot(ROLE_MELEE) &&
                !DraftBestBot(ROLE_HEALER) && !DraftBestBot(ROLE_TANK) &&
                !DraftBestBot(ROLE_RANGED, 0, false) && !DraftBestBot(ROLE_MELEE, 0, false) &&
                !DraftBestBot(ROLE_HEALER, 0, false)) break;
        }

        for (auto const& c : BotRaidConfigData::CLASS_CAPS)
            if (overCap[c.cls])
                handler->PSendSysMessage("Not enough other bots: drafted {} {} past this lineup's {}.",
                    overCap[c.cls], overCap[c.cls] == 1 ? c.singular : c.plural, q.*c.count);

        if (!group) {
            group = new Group();
            if (!group->Create(player)) {
                delete group; 
                handler->SendSysMessage("Critical Error: Core failed to instantiate Group object.");
                return true;
            }
            sGroupMgr->AddGroup(group);
        }
        
        if (size > 5 && !group->isRaidGroup()) group->ConvertToRaid();

        bool addedAny = false;
        for (Player* bot : draftedBots) {
            if (group->IsFull()) break;
            
            if (group->AddMember(bot)) {
                addedAny = true;
                {
                    std::lock_guard<std::mutex> lock(s_draftedBotsLock);
                    s_draftedBots[player->GetGUID()].insert(bot->GetGUID());
                }
                if (bot->GetMapId() != player->GetMapId() || !bot->IsWithinDistInMap(player, 40.0f)) {
                    bot->CombatStop(true);
                    bot->TeleportTo(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), 
                                    player->GetPositionZ(), player->GetOrientation(), 0, player);
                }
            }
        }

        if (addedAny)
            StartCooldown(player, s_lastAssemble);

        if (isTele) {
            teleLog << "\n--- POST-ASSEMBLY BOT STATES (FUNCTIONAL VERIFICATION) ---\n";
            for (Player* bot : draftedBots) {
                AppendBotStateTelemetry(teleLog, bot);
                teleLog << "\n";
            }
            
            teleLog << "--- FINAL MATRIX ---\n"
                    << "Final Draft Size: " << draftedBots.size() << "\n"
                    << "Final Buff Matrix Hash: 0x" << std::hex << currentRaidBuffs << std::dec << "\n"
                    << "========================================================\n";

            std::string logDir = sConfigMgr->GetOption<std::string>("LogsDir", ".");
            if (logDir.back() != '/' && logDir.back() != '\\') logDir += "/";
            
            std::time_t now = std::time(nullptr);
            char timeBuf[64];
            std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", std::localtime(&now));
            
            std::string filename = logDir + "botraid_telemetry_assembly_" + std::string(timeBuf) + "_" + std::to_string(size) + "man.log";
            std::ofstream outFile(filename);
            
            if (outFile.is_open()) {
                outFile << teleLog.str();
                outFile.close();
                handler->PSendSysMessage("Telemetry airgap log successfully exported to: {}", filename);
            } else {
                handler->SendSysMessage("TELEMETRY ERROR: Failed to open output file in Logs directory.");
            }
        }

        handler->PSendSysMessage("Optimal {}-man raid dynamically scaled and assembled.", size);

        if (cfg->sortGroups && group->isRaidGroup())
            SortRaidGroups(group, handler);
        return true;
    }

    // ==============================================================================
    // RAID SUBGROUP SORTING
    // ==============================================================================

    // Order raid members are laid out in, group 1 first.
    enum SortType : uint8 { SORT_TANK, SORT_MELEE, SORT_HUNTER, SORT_CASTER, SORT_HEALER, SORT_TYPES };

    static char const* SortTypeName(uint8 type) {
        switch (type) {
            case SORT_TANK:   return "tank";
            case SORT_MELEE:  return "melee";
            case SORT_HUNTER: return "hunter";
            case SORT_CASTER: return "caster";
            default:          return "healer";
        }
    }

    static uint8 GetSortType(Player* p) {
        BotRole role; float comp; uint32 buffs;
        MapBotProfile(p, GetBotSpec(p, p->getClass()), role, comp, buffs);
        switch (role) {
            case ROLE_TANK:   return SORT_TANK;
            case ROLE_HEALER: return SORT_HEALER;
            case ROLE_MELEE:  return SORT_MELEE;
            default:          return p->getClass() == CLASS_HUNTER ? SORT_HUNTER : SORT_CASTER;
        }
    }

    // How much a group gains from a shaman's totems, by the kind of players in it: Windfury and
    // Strength of Earth for melee, agility for hunters, Wrath of Air for casters, Mana Spring for
    // healers.
    static int TotemValue(uint8 type) {
        switch (type) {
            case SORT_MELEE:  return 4;
            case SORT_HUNTER: return 3;
            case SORT_CASTER: return 2;
            case SORT_HEALER: return 1;
            default:          return 0;
        }
    }

    // Sorts a raid's online members into subgroups: tanks in group 1, then melee, hunters, casters
    // and healers, so party-only effects (totems, Prayer of Healing, Vampiric Embrace, Blood Pact)
    // land on the players who use them. Totems only reach their own group and don't depend on spec,
    // so shamans are placed first, one per group, in the groups that gain the most (melee, then
    // hunters, casters, healers), matching spec to group where possible; everyone else is laid
    // out around them. Offline members keep their slots. Returns how many members moved.
    static uint32 SortRaidGroups(Group* group, ChatHandler* handler)
    {
        constexpr uint8 SUBGROUPS = MAXRAIDSIZE / MAXGROUPSIZE;
        if (!group || !group->isRaidGroup())
            return 0;

        struct Entry { Player* player; uint8 type; bool shaman; uint8 subgroup; bool placed; };
        std::vector<Entry> members;
        uint8 capacity[SUBGROUPS];
        std::fill(std::begin(capacity), std::end(capacity), uint8(MAXGROUPSIZE));

        for (Group::MemberSlot const& slot : group->GetMemberSlots()) {
            Player* p = ObjectAccessor::FindConnectedPlayer(slot.guid);
            if (p && p->GetGroup() == group)
                members.push_back({ p, GetSortType(p), p->getClass() == CLASS_SHAMAN, 0, false });
            else if (slot.group < SUBGROUPS && capacity[slot.group] > 0)
                --capacity[slot.group];
        }
        if (members.empty())
            return 0;

        std::stable_sort(members.begin(), members.end(),
            [](Entry const& a, Entry const& b) { return a.type < b.type; });

        // Fills subgroups in order, skipping placed members. False if they don't fit.
        auto layOut = [&](uint8 const (&cap)[SUBGROUPS]) {
            uint8 sub = 0, used = 0;
            for (Entry& e : members) {
                if (e.placed)
                    continue;
                while (sub < SUBGROUPS && used >= cap[sub]) { ++sub; used = 0; }
                if (sub >= SUBGROUPS)
                    return false;
                e.subgroup = sub;
                ++used;
            }
            return true;
        };

        // Pass 1: plain layout, to learn what kind of group each one is.
        if (!layOut(capacity))
            return 0;  // more online members than free slots; can't happen in a valid raid
        uint8 groupCount = 0;
        for (Entry const& e : members)
            groupCount = std::max<uint8>(groupCount, e.subgroup + 1);

        uint8 kind[SUBGROUPS] = {};
        for (uint8 g = 0; g < groupCount; ++g) {
            uint8 counts[SORT_TYPES] = {};
            for (Entry const& e : members)
                if (e.subgroup == g) ++counts[e.type];
            kind[g] = uint8(std::max_element(std::begin(counts), std::end(counts)) - std::begin(counts));
        }

        // Pass 2: one shaman per group, best groups first; a shaman of the group's own kind if
        // there is one, otherwise any.
        std::vector<uint8> order;
        for (uint8 g = 0; g < groupCount; ++g)
            order.push_back(g);
        std::stable_sort(order.begin(), order.end(),
            [&](uint8 a, uint8 b) { return TotemValue(kind[a]) > TotemValue(kind[b]); });

        size_t shamans = std::count_if(members.begin(), members.end(), [](Entry const& e) { return e.shaman; });
        if (order.size() > shamans)
            order.resize(shamans);

        uint8 remaining[SUBGROUPS];
        std::copy(std::begin(capacity), std::end(capacity), std::begin(remaining));
        std::vector<uint8> unfilled;
        auto place = [&](Entry& shaman, uint8 g) {
            shaman.placed = true;
            shaman.subgroup = g;
            --remaining[g];
        };
        for (uint8 g : order) {
            auto match = std::find_if(members.begin(), members.end(),
                [&](Entry const& e) { return e.shaman && !e.placed && e.type == kind[g]; });
            if (match != members.end())
                place(*match, g);
            else
                unfilled.push_back(g);
        }
        for (uint8 g : unfilled) {
            auto any = std::find_if(members.begin(), members.end(),
                [](Entry const& e) { return e.shaman && !e.placed; });
            if (any != members.end())
                place(*any, g);
        }

        // Pass 3: everyone else (including shamans beyond one per group) around them.
        if (!layOut(remaining))
            return 0;

        uint32 changed = 0;
        for (Entry const& e : members) {
            if (group->GetMemberGroup(e.player->GetGUID()) != e.subgroup) {
                group->ChangeMembersGroup(e.player->GetGUID(), e.subgroup);
                ++changed;
            }
        }

        if (handler) {
            handler->PSendSysMessage("Raid sorted into {} groups ({} moved):", groupCount, changed);
            for (uint8 g = 0; g < groupCount; ++g) {
                std::string line;
                for (Entry const& e : members) {
                    if (e.subgroup != g) continue;
                    line += (line.empty() ? "" : ", ") + e.player->GetName() + " (" + SortTypeName(e.type) +
                        (e.shaman ? " shaman" : "") + ")";
                }
                if (!line.empty())
                    handler->PSendSysMessage("  Group {}: {}", g + 1, line);
            }
        }
        return changed;
    }

    static bool HandleSort(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        Group* group = player->GetGroup();
        if (!group || !group->isRaidGroup()) {
            handler->SendSysMessage("You need to be in a raid group to sort it.");
            return true;
        }
        if (!group->IsLeader(player->GetGUID()) && !group->IsAssistant(player->GetGUID())) {
            handler->SendSysMessage("Only the raid leader or an assistant can sort the raid.");
            return true;
        }
        SortRaidGroups(group, handler);
        return true;
    }

    static bool IsRaidMap(uint32 mapId)
    {
        MapEntry const* entry = sMapStore.LookupEntry(mapId);
        return entry && entry->IsRaid();
    }

    // Clears raid lockouts of the random bots in the caller's group, so they can follow the
    // leader into a fresh instance. Alt bots are real characters and keep their lockouts.
    static bool HandleUnbind(ChatHandler* handler, Optional<std::string> optArg)
    {
        Player* player = handler->GetSession()->GetPlayer();
        Group* group = player->GetGroup();
        if (!group) {
            handler->SendSysMessage("You need to be in a group with bots.");
            return true;
        }
        if (!group->IsLeader(player->GetGUID()) && !group->IsAssistant(player->GetGUID())) {
            handler->SendSysMessage("Only the group leader or an assistant can clear bot lockouts.");
            return true;
        }

        bool all = false;
        uint32 mapId = 0;
        std::string raidName;
        if (optArg) {
            std::string arg = *optArg;
            std::transform(arg.begin(), arg.end(), arg.begin(), ::tolower);
            if (arg == "all") {
                all = true;
                raidName = "any raid";
            } else if (auto const* encounter = BotRaidConfigData::instance()->FindEncounter(arg)) {
                mapId = encounter->mapId;
                raidName = encounter->name;
            } else {
                handler->PSendSysMessage("Unknown raid '{}'. Type .botraid raids for the short names.", *optArg);
                return true;
            }
        } else if (IsRaidMap(player->GetMapId())) {
            mapId = player->GetMapId();
            MapEntry const* entry = sMapStore.LookupEntry(mapId);
            raidName = entry->name[handler->GetSessionDbcLocale()];
        } else {
            handler->SendSysMessage("Syntax: .botraid unbind <raid|all>, for example .botraid unbind mc. Inside a raid, .botraid unbind clears that raid.");
            return true;
        }

        uint32 cleared = 0, botsCleared = 0, keptInside = 0;
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next()) {
            Player* bot = itr->GetSource();
            if (!bot || bot == player || !PlayerbotsMgr::instance().GetPlayerbotAI(bot) || !sRandomPlayerbotMgr.IsRandomBot(bot))
                continue;

            bool any = false;
            for (uint8 d = 0; d < MAX_DIFFICULTY; ++d) {
                Difficulty difficulty = Difficulty(d);
                // Collect first: unbinding erases from the map being iterated.
                std::vector<uint32> maps;
                for (auto const& [boundMap, bind] : sInstanceSaveMgr->PlayerGetBoundInstances(bot->GetGUID(), difficulty))
                    if (all ? IsRaidMap(boundMap) : boundMap == mapId)
                        maps.push_back(boundMap);

                for (uint32 boundMap : maps) {
                    if (bot->GetMapId() == boundMap) {
                        ++keptInside;
                        continue;
                    }
                    sInstanceSaveMgr->PlayerUnbindInstance(bot->GetGUID(), boundMap, difficulty, true, bot);
                    ++cleared;
                    any = true;
                }
            }
            if (any)
                ++botsCleared;
        }

        if (cleared)
            handler->PSendSysMessage("Cleared {} lockout(s) for {} on {} bot(s).", cleared, raidName, botsCleared);
        else if (!keptInside)
            handler->PSendSysMessage("None of your group's random bots are locked to {}.", raidName);
        if (keptInside)
            handler->PSendSysMessage("{} lockout(s) kept because those bots are inside the raid. Have them leave it first.", keptInside);
        return true;
    }

    static bool HandleDismiss(ChatHandler* handler, Optional<std::string> optArg)
    {
        if (optArg) {
            std::string argStr = *optArg;
            std::transform(argStr.begin(), argStr.end(), argStr.begin(), ::tolower);
            if (argStr == "freeroam") {
                return ExecuteDismiss(handler, true);
            }
            
            handler->SendSysMessage("Syntax: .botraid dismiss [freeroam]");
            return true;
        }
        
        return ExecuteDismiss(handler, false);
    }

    static bool ExecuteDismiss(ChatHandler* handler, bool freeroam)
    {
        if (OnCooldown(handler, s_lastDismiss, "dismiss"))
            return true;

        Player* player = handler->GetSession()->GetPlayer();
        Group* initialGroup = player->GetGroup();

        if (!initialGroup || initialGroup->GetLeaderGUID() != player->GetGUID()) {
            handler->SendSysMessage("You must be the group leader to dismiss the mercenaries.");
            return true;
        }

        std::ostringstream teleLog;
        bool isTele = s_telemetryEnabled;

        if (isTele) {
            teleLog << "========================================================\n"
                    << "         OPTIMAL BOT RAID - DISMISSAL TELEMETRY         \n"
                    << "========================================================\n"
                    << "Leader: " << player->GetName() << "\n"
                    << "Mode: " << (freeroam ? "Free-roam (No Teleport)" : "Standard (Homebind Teleport)") << "\n\n"
                    << "--- PRE-DISMISSAL BOT STATES (IDENTIFIED FOR REMOVAL) ---\n";
        }

        // Only the bots this leader drafted; manually invited bots and alt bots stay.
        // Taken out under the lock and released before the bots leave the group, whose hooks
        // take it again.
        std::unordered_set<ObjectGuid> drafted;
        {
            std::lock_guard<std::mutex> lock(s_draftedBotsLock);
            auto draftedItr = s_draftedBots.find(player->GetGUID());
            if (draftedItr != s_draftedBots.end()) {
                drafted = std::move(draftedItr->second);
                s_draftedBots.erase(draftedItr);
            }
        }

        std::vector<ObjectGuid> botsToRemove;
        for (GroupReference* itr = initialGroup->GetFirstMember(); itr != nullptr; itr = itr->next()) {
            if (Player* member = itr->GetSource()) {
                if (member != player && drafted.count(member->GetGUID()) && PlayerbotsMgr::instance().GetPlayerbotAI(member)) {
                    botsToRemove.push_back(member->GetGUID());
                    if (isTele) {
                        AppendBotStateTelemetry(teleLog, member);
                        teleLog << "\n";
                    }
                }
            }
        }

        if (botsToRemove.empty()) {
            handler->SendSysMessage("You have no drafted mercenaries in your group.");
            return true;
        }

        StartCooldown(player, s_lastDismiss);

        if (isTele) teleLog << "--- EXECUTING TEARDOWN SEQUENCE ---\n";

        for (ObjectGuid guid : botsToRemove) {
            if (Player* bot = ObjectAccessor::FindConnectedPlayer(guid)) {
                
                if (isTele) {
                    bool isRandom = sRandomPlayerbotMgr.IsRandomBot(bot);
                    teleLog << "Processing Dismissal for: " << bot->GetName() << "\n"
                            << "  Logic Branch: " << (isRandom ? "System RandomBot (Factory Refresh Scheduled)" : "Player Alt (Manual Cleanse Scheduled)") << "\n";
                    
                    if (freeroam) {
                        teleLog << "  Relocating to Homebind: [SKIPPED - FREEROAM MODE ACTIVE]\n\n";
                    } else {
                        teleLog << "  Relocating to Homebind MapId: " << bot->m_homebindMapId 
                                << " (X: " << bot->m_homebindX << ", Y: " << bot->m_homebindY << ", Z: " << bot->m_homebindZ << ")\n\n";
                    }
                }

                if (Group* currentGroup = bot->GetGroup()) {
                    currentGroup->RemoveMember(guid, GROUP_REMOVEMETHOD_DEFAULT, player->GetGUID());
                }

                if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot)) {
                    ai->SetMaster(nullptr);

                    ai->AddTimedEvent([guid]() {
                        Player* b = ObjectAccessor::FindConnectedPlayer(guid);
                        if (!b) return;
                        
                        PlayerbotAI* bAi = PlayerbotsMgr::instance().GetPlayerbotAI(b);
                        if (!bAi) return;

                        bool isRandomBot = sRandomPlayerbotMgr.IsRandomBot(b);

                        if (isRandomBot) {
                            // Restock and repair only; keep the bot's gear and talents.
                            PlayerbotFactory factory(b, b->GetLevel());
                            factory.Refresh();
                            
                            bAi->ResetStrategies(false);
                            bAi->ChangeStrategy("+roam", BOT_STATE_NON_COMBAT);
                            bAi->Reset(true);
                        } else {
                            bAi->ResetStrategies(false);
                            
                            const char* strats[] = { "-follow", "-dps assist", "-assist", "-tank assist", "-healer dps" };
                            for (const char* strat : strats) {
                                bAi->ChangeStrategy(strat, BOT_STATE_NON_COMBAT);
                                bAi->ChangeStrategy(strat, BOT_STATE_COMBAT);
                            }
                            
                            bAi->ChangeStrategy("+roam", BOT_STATE_NON_COMBAT);
                            bAi->Reset(true);
                        }
                    }, 1000); 
                }
                
                bot->StopMoving();
                if (MotionMaster* mm = bot->GetMotionMaster()) {
                    mm->Clear();
                }

                // If user didn't flag for freeroam, send bots back to their respective inns
                if (!freeroam) {
                    bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ, 0.0f);
                }
            }
        }

        if (isTele) {
            teleLog << "========================================================\n"
                    << "Dismissal operation complete.\n";

            std::string logDir = sConfigMgr->GetOption<std::string>("LogsDir", ".");
            if (logDir.back() != '/' && logDir.back() != '\\') logDir += "/";
            
            std::time_t now = std::time(nullptr);
            char timeBuf[64];
            std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", std::localtime(&now));
            
            std::string filename = logDir + "botraid_telemetry_dismissal_" + std::string(timeBuf) + ".log";
            std::ofstream outFile(filename);
            
            if (outFile.is_open()) {
                outFile << teleLog.str();
                outFile.close();
                handler->PSendSysMessage("Telemetry dismissal airgap log exported to: {}", filename);
            } else {
                handler->SendSysMessage("TELEMETRY ERROR: Failed to open output file in Logs directory.");
            }
        }

        if (freeroam) {
            handler->SendSysMessage("Dismissed all bot mercenaries. They have been cut loose in their current location.");
        } else {
            handler->SendSysMessage("Dismissed all bot mercenaries. They have returned to their duties.");
        }
        
        return true;
    }
};

void Addcs_optimal_bot_raidScripts() {
    new cs_optimal_bot_raid();
    new OptimalBotRaidConfigScript();
    new OptimalBotRaidGroupScript();
}