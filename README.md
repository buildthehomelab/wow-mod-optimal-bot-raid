# 🤖 Optimal Bot Raid (mod-optimal-bot-raid)

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL%20v3-blue.svg)](https://www.gnu.org/licenses/agpl-3.0)
[![Core: AzerothCore](https://img.shields.io/badge/Core-AzerothCore%203.3.5a-red.svg)](https://github.com/azerothcore/azerothcore-wotlk)
[![Dependency: Playerbots](https://img.shields.io/badge/Dependency-Playerbots-orange.svg)]()

An advanced, mathematically optimized C++ raid orchestration module for **AzerothCore (3.3.5a)** and **mod-playerbots** (Tested against Playerbot core `3fa1c1e4`).

Tired of manually drafting, whispering, and grouping bots to ensure you have the right buffs for a raid? This module introduces a highly performant C++ algorithm that instantly scans your server's entire bot population, solves the **Set Cover** (raid buff synergies) and **Knapsack** (GearScore/AI competency) optimization problems, and teleports the mathematically perfect raid composition directly to you.

> **Fork note.** This is the [buildthehomelab](https://github.com/buildthehomelab) fork of
> [barnaclebarry/mod-optimal-bot-raid](https://github.com/barnaclebarry/mod-optimal-bot-raid), changed for a
> public server where every player can use `.botraid`:
>
> * Players can't draft bots more than `OptimalBotRaid.MaxLevelAbovePlayer` levels above themselves (default 2), or past the end of their 60/70/80 bracket. Custom ranges are clamped; GMs are exempt.
> * At level 60, 70 and 80 players only draft bots of exactly their level (`OptimalBotRaid.SameLevelAtCaps`), with no lower-level fill-ins.
> * Only random bots are drafted, never another player's alt bots, and bots queued for LFG/battlegrounds or inside an instance are skipped.
> * `.botraid dismiss` only releases the bots `.botraid assemble` drafted for you. Bots you invited yourself and your alt bots stay in the group.
> * Dismissed random bots are restocked and repaired, not re-randomized, so they keep their gear and talents.
> * `.botraid telemetry` and `.botraid debug` are GM-only.

## Patch Notes: Bot Raid Assembly

Category: Bots

- Type **.botraid assemble 10** to fill your group with bots picked for the right tanks, healers and raid buffs. They join you right away, wherever you are. Sizes 5, 10, 15, 20, 25 and 40 are supported.
- Bots are drafted around your level and never more than 2 levels above you. You can ask for a range, for example **.botraid assemble 55-58 10**.
- At level 60, 70 and 80 you only draft bots of your own level.
- Type **.botraid dismiss** to send the bots you drafted home. Bots you invited yourself stay.
- **Onyxia lineup:** assembling inside Onyxia's Lair, or typing **.botraid assemble onyxia 40** (or 10, 25) anywhere, drafts a ranged-heavy raid, since only ranged can hit Onyxia while she's flying. Works for the 10/25-man and the 40-man version.
- **Molten Core lineup:** assembling inside Molten Core, or typing **.botraid assemble mc** anywhere, drafts 5 tanks for Garr, Golemagg and Majordomo's adds, 11 healers, and fewer melee, since Ragnaros knocks melee into the lava.

---

## ⚠️ Critical Dependency Warning

**This module has a hard dependency on the Playerbot autonomous agent framework.**  
Compilation will safely bypass and disable the module on a vanilla AzerothCore deployment to prevent core breakage. The `CMakeLists.txt` demands explicit access to `PlayerbotAI.h`, `RandomPlayerbotMgr.h`, and `PlayerbotFactory.h` APIs. It will actively scan your source tree (checking `/src/server/game/AI/Playerbot/` and `/modules/mod-playerbots/src/Bot/`) and will automatically halt compilation of this specific module if the framework is missing.

---

## ✨ Features

* **Instant Autonomous Assembly:** Drafts, invites, teleports, and syncs AI state machines for parties and raids of 5, 10, 15, 20, 25, or 40 players in a single command.
* **WotLK Buff Synergy:** Uses lightning-fast bitwise matrix math to guarantee your raid is drafted with 10% Stats, Replenishment, Bloodlust, 13% Magic Damage, etc., with zero buff redundancy. Special mathematical weights are natively given to heavily sought-after WotLK buffs.
* **AI Competency Multipliers:** The algorithm heavily favors bot specs that perform flawlessly (e.g., Holy Paladins at `1.5x`, Combat Rogues at `1.5x`) while mathematically penalizing specs that bots struggle with (e.g., Unholy DKs dropping to `0.6x`, Feral Druids to `0.5x`).
* **Progression Agnostic & Intelligent Bounds:** You can request explicitly tight level boundaries (e.g., `60-67`). If the server doesn't have enough idle bots matching those exact constraints, the C++ algorithm dynamically and smoothly relaxes the limit downward (as far as level 10) to safely salvage the draft and fill out your roster.
* **Crash-Proof Concurrency & Thread Safety:** Polling a server's entire player array can cause severe memory access violations. This module natively utilizes `HashMapHolder<Player>::GetLock()` with a `std::shared_lock<std::shared_mutex>` to ensure thread-safe asynchronous entity scraping and evaluation.
* **Player Alt Preservation vs. Random Bots:** Intelligently differentiates between system-generated Random Bots and offline Player Alts. When dismissing an Alt, it carefully strips specific raid strategies without invoking a `PlayerbotFactory::Refresh()`, ensuring real players' gear, UI, and custom specs are never accidentally wiped by the factory.

---

## 🚀 Installation

This module is built to standard AzerothCore specifications and must be compiled into your core alongside `mod-playerbots`.

1. Navigate to your AzerothCore source directory:
   ```bash
   cd azerothcore-wotlk/modules
   ```
2. Clone this repository (or copy the mod-optimal-bot-raid directory in):
   ```bash
   git clone https://github.com/buildthehomelab/wow-mod-optimal-bot-raid.git mod-optimal-bot-raid
   ```
3. Re-run CMake to generate the build files, and compile:
   ```bash
   cd ../build
   cmake ../ -DCMAKE_INSTALL_PREFIX=$(pwd)/install
   make -j$(nproc)
   ```
   *(Note: If compiling on a memory-constrained device like a Raspberry Pi, use `make -j1` or `make -j2` to avoid linking crashes).*

Once compiled, any `.conf.dist` files are automatically deployed to your server's `etc/modules` directory.

---

## 🎮 How to Use (In-Game Commands)

Once compiled and your server is running, log into any character (Level 1-80) and use the following chat commands. You must be alive to use these commands.

### 1. Assemble a Group or Raid
**Syntax:** `.botraid assemble [<min-max>] <size>`  
**Supported Sizes:** 5, 10, 15, 20, 25, 40  
**Examples:** `.botraid assemble 10` | `.botraid assemble 60-67 40` | `.botraid assemble onyxia 40` | `.botraid assemble mc`  
**What it does:**
* Evaluates your current group (and any human friends currently with you).
* Calculates missing roles based on strict Ranged-to-Melee bias quotas to prevent melee cleave deaths.
* Scans the server safely for idle, out-of-combat bots matching your level bracket. If a custom bracket is not provided, defaults to 4 levels below the leader up to `OptimalBotRaid.MaxLevelAbovePlayer` levels above (custom brackets are clamped to the same cap; GMs are exempt). Only random bots that aren't queued for LFG/battlegrounds or inside an instance are considered.
* **Intelligent Relaxation:** If there are not enough bots in the desired bracket, it will dynamically and gradually relax the minimum level limit down (as far as level 10) to fulfill the draft, while explicitly notifying you of the adjusted range.
* Scores them based on their Normalized GearScore and the missing unique buffs they bring to your current comp.
* Invites them, converts the group to a raid, teleports them to your exact coordinates/instance, and resets their AI to follow you.

### 2. Dismiss the Mercenaries
**Syntax:** `.botraid dismiss [freeroam]`  
**What it does:**
* Removes the bots `.botraid assemble` drafted for you from your current group. Human players, bots you invited yourself and your alt bots are untouched. The list is kept in memory, so it is lost on a server restart.
* Severs the AI master link and halts physics/momentum natively via `bot->StopMoving()` and `MotionMaster->Clear()`.
* If `freeroam` is specified, the script cuts the bots loose right in their current spot without natively teleporting them back to their home Innkeeper bind.
* **Defeats Asynchronous AI Race Conditions:** Uses a 1000ms `AddTimedEvent()` delayed lambda callback. This intentionally bypasses the notorious `ResetAiAction` race condition inherent to the Playerbot core, ensuring mercenaries accurately decouple from the raid leader without freezing into un-targetable statues.
* **Executes a Split Strategy Cleanse:**
  * **System RandomBots** are factory refreshed (restocked, repaired), keeping their gear and talents.
  * **Offline Player Alts** receive manual strategy stripping (`-follow`, `-dps assist`, etc.) to protect their customized gear.
  * Both are forced into `+roam` state and are naturally removed.

### 3. Generate Telemetry (Debug Mode, GM only)
**Syntax:** `.botraid debug`  
*(Requires you to target a bot)*  
**What it does:**
* Extracts real-time C++ diagnostic state data from the selected bot (MotionMaster Type, Unit State, Combat State, AI Master links, and loaded Strategy arrays).
* Dumps the payload to `botraid_debug.log` located inside your server's configured `LogsDir` (or the container's mounted log volume). Crucial for server admins tracking down frozen AI states or strategy loops.

### 4. Check Module Version
**Syntax:** `.botraid version`  
**What it does:**
* Prints the C++ compilation date and time to verify the binary mapped against your AzerothCore environment.

---

## ⚙️ How the Algorithm Works (Under the Hood)

If you are curious about how the module decides who to invite, it uses a **Dynamic Min-Max Normalized Greedy Heuristic**.

1. **Spec Identification:** Bots do not actively broadcast their specialization. The algorithm maps known capstone talent spells (e.g., Chaos Bolt, Divine Storm, Earth Shield) to perfectly identify their exact WotLK spec.
2. **The Base Score:** The algorithm calculates a bot's AverageItemLevel and normalizes it against the highest geared eligible bot currently in the pool, so raw GearScore doesn't mathematically overshadow the value of unique buffs.
3. **The AI Multiplier:** The normalized GS is multiplied by a static competency modifier defined in the C++ tree mappings (`MapBotProfile`). (e.g., A Destro Warlock gets a `1.5x` multiplier, while a Disc Priest gets a `0.6x` multiplier due to predictive shielding logic).
4. **The Synergy Bonus:** The algorithm evaluates exactly how many new WotLK buffs that spec brings to the group's current bitwise buff matrix (`~currentRaidBuffs & pool[i].providedBuffs`).
5. **The Final Mathematical Weighting:**
   `Final Score = (normalizedGS * 0.5) + (buffScore * 0.5)`
   *(Where buffScore adds 0.2 per unique buff, with a massive +0.4 overriding bonus for Bloodlust and +0.2 for Replenishment).*
6. **Drafting & Re-evaluating:** Once a bot is drafted, the raid's "buff matrix" updates via bitwise OR assignments (`currentRaidBuffs |= pool[bestIndex].providedBuffs`). The remaining bots in the pool are instantly re-scored. (e.g., If the first bot drafted was a Shaman, the algorithm instantly devalues all other Shamans in the pool because the raid already possesses `BUFF_BLOODLUST`).

### Target Role Quotas by Raid Size
The algorithm strictly enforces the following quotas to ensure boss survivability, deliberately prioritizing ranged DPS over melee to prevent cleave-mechanic wipes:

| Raid Size | Tanks | Healers | Melee (Max) | Ranged |
| :--- | :--- | :--- | :--- | :--- |
| **5-man** | 1 | 1 | 1 | 2 |
| **10-man** | 2 | 2 | 2 | 4 |
| **15-man** | 2 | 3 | 3 | 7 |
| **20-man** | 2 | 4 | 5 | 9 |
| **25-man** | 2 | 5 | 6 | 12 |
| **40-man (Vanilla <= Lvl 60)** | 4 | 10 | 10 | 16 |
| **40-man (WotLK > Lvl 60)** | 3 | 8 | 10 | 19 |

### Raid Lineups
Used instead of the table above when the leader assembles inside the raid, or anywhere with
`.botraid assemble <raid> [size]`. Sizes without a lineup fall back to the standard one. A raid
with a single size (Molten Core) doesn't need the size. Configurable under
`OptimalBotRaid.Quota.<Raid>.<size>.*`.

| Raid | Command | Tanks | Healers | Melee (Max) | Ranged |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Onyxia 10-man** | `onyxia 10` / `ony 10` | 2 | 2 | 1 | 5 |
| **Onyxia 25-man** | `onyxia 25` | 3 | 5 | 3 | 14 |
| **Onyxia 40-man (individual-progression)** | `onyxia 40` | 4 | 10 | 4 | 22 |
| **Molten Core** | `mc` / `moltencore` | 5 | 11 | 8 | 16 |

* **Onyxia:** her air phase (65% to 40% health) is out of melee reach, so these draft as much
  ranged as the raid can carry. The extra tank picks up the Onyxian Lair Guards (10/25) and whelp
  waves (40).
* **Molten Core:** classic guides run 3-5 tanks and 10-12 healers
  ([Warcraft Tavern](https://www.warcrafttavern.com/wow-classic/guides/mc/),
  [Boosting Ground](https://boosting-ground.com/wow-classic/guides/raid-guides/molten-core-guide)).
  Garr, Golemagg (two Core Ragers) and Majordomo (eight adds) need extra tanks, and Ragnaros's Wrath
  knocks melee back into the lava, so this takes the top of both ranges and fewer melee than the
  stock vanilla 40.

---

## ⚠️ Edge Cases, Safeties, & Limitations

* **"Not enough eligible idle bots found!"**
  If you see this error, your server does not have enough random bots enabled, or all the bots in your specific level bracket are currently dead, in combat, flying, or are ghosts. The module strictly avoids grabbing bots mid-combat (`bot->IsInCombat()`) to protect their AI memory states and server stability. **Fix:** Increase your `RandomBot` count in `aiplayerbot.conf`.
* **Human Player Overrides & Overflow Protection:**
  The script respects human players first. If you are forming a 10-man raid and you invite 3 human friends who are all playing Tanks, the script will mathematically deduct them, realize no more tanks are needed, and spend the remaining 6 bot slots purely on Healers and DPS. It dynamically trims quotas to ensure the instance size cap is never broken.
* **Instance Teleportation:**
  Bots are teleported natively using your exact MapId pointer. If you use the `.botraid assemble` command while standing inside Naxxramas, the bots will safely appear directly next to you inside the instance. You do not need to walk them through the portal.
* **Raid Downgrading:**
  You cannot type `.botraid assemble 5` if you are currently in a 10-man raid instance or a Raid Group. The core will enforce Blizzard API rules and ask you to disband your raid first to form a standard 5-man party.

---

## 🛠️ Architecture & Script Hooks

For developers and contributors, this module hooks into the following core systems natively:

* **CommandScript** (`cs_optimal_bot_raid`): Registers the `.botraid` logic table (assemble, dismiss, debug, version).
* **Concurrency Hooks:** Directly requests thread-safe locks via `HashMapHolder<Player>::GetLock()` to pause background task modifications when polling global online entities.
* **AI Interception:** Hooks into `PlayerbotsMgr` to assess and purge active combat parameters. Exploits delayed `PlayerbotAI::AddTimedEvent()` callbacks to sidestep asynchronous `ResetAiAction` race conditions on bot dismissals. Implements explicit diagnostic tracking writing custom telemetry hooks to standard `worldserver` log channels.

---

## 📄 License
This project is licensed under the GNU AGPL v3.0 - see the LICENSE file for details.
