/*
 * mod-quest-map
 *
 * The 3.3.5 client only knows the creatures and objects the server has sent it, the ones within
 * visibility range (100 yards on a continent), so a map addon can't show where quests are to be
 * had. This module answers it with the quest givers on the zone map it shows whose quests the
 * player can pick up now, checked the way the core decides on a giver's yellow or blue "!".
 *
 * Wire format, both directions: "<COMMAND>:<request id>:<field>:<field>..." after the addon
 * prefix and a tab. Rows inside a field are separated by ';' and their values by ','.
 *
 *   HELLO:<req>:<protocol>   -> HI:<req>:<version>:<flags>
 *   A:<req>:<map area>       -> AR:<req>:<map area>:<givers>, any number of AD rows, AE:<req>
 *       G,<n>,<x>,<y>,<name>                   a quest giver; x and y in 1/10000 of the map
 *       Q,<n>,<quest>,<level>,<flags>,<title>  one of giver n's quests (QuestFlags)
 *
 * <map area> is a WorldMapArea.dbc id: GetCurrentMapAreaID() - 1 on the client. The answers use
 * other words than the requests because without this module the server hands the addon its own
 * whisper back, and that must never read as an answer.
 *
 * Addon messages arrive as CMSG_MESSAGECHAT on the world thread, so requests are answered
 * straight from the hook.
 *
 * Released under the MIT License.
 */

#include "Chat.h"
#include "ConditionMgr.h"
#include "Config.h"
#include "DBCFileLoader.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameEventMgr.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace QuestMap
{
    namespace
    {
        // Addon message prefix; the client sends "QMP\t<command>" as a whisper to itself.
        constexpr char const* PREFIX = "QMP";

        // Bumped when a message changes shape. The addon ignores the server on a mismatch.
        constexpr uint32 PROTOCOL_VERSION = 1;

        // Leaves room for the prefix and tab inside the client's 255-byte chat message limit.
        constexpr std::size_t MAX_PAYLOAD = 240;

        // Capability bits in the HI answer.
        enum HelloFlags : uint32
        {
            HELLO_GIVERS = 0x1, // A lists a map's quest givers
        };

        // The flags field of a Q row.
        enum QuestFlags : uint32
        {
            QUEST_REPEATABLE = 0x1, // a daily or repeatable quest: a blue "!"
            QUEST_TRIVIAL    = 0x2, // far below the player's level: the core greys its "!"
        };

        // More givers than this on one map are left out (a capital city has about sixty).
        constexpr std::size_t MAX_GIVERS = 200;

        // Two spawns of one giver closer than this share of the map are one pin.
        constexpr float SAME_PLACE = 0.01f;

        // Each answer walks a map's givers and their quests on the world thread, and addon
        // whispers are never muted by the chat flood check, so each character gets a small budget.
        constexpr float REQUEST_BURST = 8.0f;
        constexpr float REQUEST_RATE = 1.0f;

        constexpr uint32 IP_PROGRESSION_QUEST_BASE = 66000;
        constexpr uint8 IP_STATE_MAX = 18;
        constexpr uint8 IP_STATE_TBC = 8;
        constexpr uint8 IP_STATE_QUEL_DANAS = 12;
        constexpr uint8 IP_STATE_WOTLK = 13;
        constexpr uint8 IP_STATE_NEVER = 255;

        constexpr uint32 MAP_OUTLAND = 530;
        constexpr uint32 MAP_EBON_HOLD = 609;
        constexpr uint32 ZONE_ISLE_OF_QUEL_DANAS = 4080;
        constexpr std::array<uint32, 6> ZONES_OPEN_ON_OUTLAND_MAP = {
            3430, // Eversong Woods
            3433, // Ghostlands
            3487, // Silvermoon City
            3524, // Azuremyst Isle
            3525, // Bloodmyst Isle
            3557, // The Exodar
        };

        // When mod-individual-progression's visibility scripts (IndividualProgressionAwareness.cpp)
        // show a creature or object: from progression state `from` and before state `before`
        // (0 = no bound). `passedBefore`: the bound is a !hasPassedProgression() check, which
        // IndividualProgression.ProgressionLimit can lift; the pre_* scripts use isBeforeProgression().
        // npc_ipp_zg and npc_ipp_za take their state from the config. The same table as
        // mod-retail-professions' (RetailProfessionsIndex.cpp).
        struct ProgressionGate
        {
            std::string_view script;
            uint8 from;
            uint8 before;
            bool passedBefore;
        };
        constexpr std::array<ProgressionGate, 26> PROGRESSION_GATES = {{
            { "gobject_ipp_preaq",    3,  0, false },
            { "gobject_ipp_aqwar",    4,  5, true },
            { "gobject_ipp_si",       6,  7, true },
            { "gobject_ipp_naxx40",   6,  0, false },
            { "gobject_ipp_pre_tbc",  0,  8, false },
            { "gobject_ipp_tbc",      8,  0, false },
            { "gobject_ipp_tbc_t4",   12, 0, false },
            { "gobject_ipp_wotlk",    13, 0, false },
            { "npc_ipp_preaq",        3,  0, false },
            { "npc_ipp_we",           3,  4, true },
            { "npc_ipp_aq",           5,  0, false },
            { "npc_ipp_aqwewar",      3,  5, true },
            { "npc_ipp_aqwar",        4,  5, true },
            { "npc_ipp_si",           6,  7, true },
            { "npc_ipp_pre_naxx40",   0,  6, false },
            { "npc_ipp_naxx40",       6,  0, false },
            { "npc_ipp_pre_tbc",      0,  8, false },
            { "npc_ipp_tbc",          8,  0, false },
            { "npc_ipp_tbc_pre_t3",   0,  10, true },
            { "npc_ipp_tbc_t3",       10, 0, false },
            { "npc_ipp_pre_wotlk",    0,  13, false },
            { "npc_ipp_wotlk",        13, 0, false },
            { "npc_ipp_wotlk_ulduar", 14, 0, false },
            { "npc_ipp_wotlk_totc",   15, 0, false },
            { "npc_ipp_wotlk_icc",    16, 0, false },
            { "npc_ipp_ds2",          3,  0, false },
        }};

        struct Config
        {
            bool enabled = true;
            bool saveZoneData = true;
            uint32 maxPinsPerGiver = 3;
            bool hideLockedContinents = true;
            bool progressionEnabled = false; // IndividualProgression.Enable
            uint8 progressionLimit = 0;      // IndividualProgression.ProgressionLimit
            uint8 zulGurubState = 3;         // IndividualProgression.RequiredZulGurubProgression
            uint8 zulAmanState = 12;         // IndividualProgression.RequiredZulAmanProgression
        };

        // One spawn of a quest giver, placed on a zone map.
        struct Giver
        {
            uint32 entry = 0;
            bool object = false;
            uint32 map = 0;
            uint32 zone = 0;
            uint16 x = 0; // 1/10000 of the map, from the left
            uint16 y = 0; // ... and from the top
            uint32 phaseMask = PHASEMASK_NORMAL;
            // The game events the spawn is listed under: > 0 out only while that one runs, < 0 gone
            // while it runs.
            std::vector<int16> events;
            // Shown only from / before these progression states (PROGRESSION_GATES); 0 = no bound.
            uint8 ipFrom = 0;
            uint8 ipBefore = 0;
        };

        // A zone map (a WorldMapArea row): the zone it draws and its corners in world coordinates.
        struct MapArea
        {
            uint32 map = 0;
            uint32 zone = 0;
            float left = 0.0f;   // world y at the map's left edge
            float right = 0.0f;
            float top = 0.0f;    // world x at the map's top edge
            float bottom = 0.0f;
            std::vector<Giver> givers;
        };

        struct SessionState
        {
            float tokens = REQUEST_BURST;
            uint32 lastRefill = 0;
        };

        Config sConfig;
        std::unordered_map<uint32, MapArea> sAreas; // WorldMapArea id -> map
        std::unordered_map<ObjectGuid::LowType, SessionState> sSessions;

        // ---- transport -------------------------------------------------------------------------

        void Send(Player* player, std::string const& payload)
        {
            if (!player || !player->GetSession())
                return;

            std::string full = std::string(PREFIX) + "\t" + payload;
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, full);
            player->GetSession()->SendPacket(&data);
        }

        // Sends "<header>:<row>;<row>;..." in as many messages as it takes, never splitting a row.
        void SendRows(Player* player, std::string const& header, std::vector<std::string> const& rows)
        {
            std::string line;
            std::size_t const budget = MAX_PAYLOAD - header.size() - 1;
            for (std::string const& row : rows)
            {
                if (!line.empty() && line.size() + 1 + row.size() > budget)
                {
                    Send(player, header + ":" + line);
                    line.clear();
                }
                if (!line.empty())
                    line += ';';
                line += row;
            }
            if (!line.empty())
                Send(player, header + ":" + line);
        }

        void SendError(Player* player, std::string const& req, std::string_view what)
        {
            Send(player, "ERR:" + req + ":" + std::string(what));
        }

        bool ParseUInt(std::string_view text, uint32& out)
        {
            if (text.empty())
                return false;
            auto result = std::from_chars(text.data(), text.data() + text.size(), out, 10);
            return result.ec == std::errc{} && result.ptr == text.data() + text.size();
        }

        // Names travel inside ','/';'/':' separated fields: those, '|' and '%' become %XX, and an
        // empty string becomes "-" (the addon's row parser drops empty values).
        std::string Escape(std::string_view text)
        {
            if (text.empty())
                return "-";

            static char const* const HEX = "0123456789ABCDEF";
            std::string out;
            out.reserve(text.size());
            for (char c : text)
            {
                if (c == ',' || c == ';' || c == ':' || c == '|' || c == '%' || c == '\n' || c == '\r')
                {
                    out += '%';
                    out += HEX[(uint8(c) >> 4) & 0xF];
                    out += HEX[uint8(c) & 0xF];
                }
                else
                    out += c;
            }
            // A lone "-" would read back as empty.
            return out == "-" ? "%2D" : out;
        }

        // A longer row than one message holds would never arrive; a title that long is cut.
        std::string Fit(std::string const& head, std::string_view name, std::size_t budget)
        {
            std::string row = head + Escape(name);
            while (row.size() > budget && !name.empty())
            {
                // Cut on a character boundary: UTF-8 continuation bytes are 10xxxxxx.
                std::size_t cut = name.size() - 1;
                while (cut > 0 && (uint8(name[cut]) & 0xC0) == 0x80)
                    --cut;
                name = name.substr(0, cut);
                row = head + Escape(name);
            }
            return row;
        }

        // ---- the index -------------------------------------------------------------------------

        void LoadSavedZones(std::unordered_map<uint32, uint32>& zones, char const* table)
        {
            if (QueryResult result = WorldDatabase.Query("SELECT CAST(guid AS UNSIGNED), CAST(zoneId AS UNSIGNED) "
                "FROM {} WHERE zoneId <> 0", table))
                do
                {
                    Field* fields = result->Fetch();
                    zones[uint32(fields[0].Get<uint64>())] = uint32(fields[1].Get<uint64>());
                } while (result->NextRow());
        }

        void LoadEvents(std::unordered_map<uint32, std::vector<int16>>& events, char const* table)
        {
            if (QueryResult result = WorldDatabase.Query("SELECT CAST(guid AS UNSIGNED), CAST(eventEntry AS SIGNED) FROM {}", table))
                do
                {
                    Field* fields = result->Fetch();
                    events[uint32(fields[0].Get<uint64>())].push_back(int16(fields[1].Get<int64>()));
                } while (result->NextRow());
        }

        // Holiday quests join the core's quest relations only while their event runs, so their
        // givers are read from the event tables; Offers() drops the quests while it's off.
        void LoadEventGivers(std::unordered_set<uint32>& givers, char const* table)
        {
            if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT CAST(id AS UNSIGNED) FROM {}", table))
                do
                {
                    givers.insert(uint32(result->Fetch()[0].Get<uint64>()));
                } while (result->NextRow());
        }

        // The progression window a spawn's mod-individual-progression script shows it in.
        void SetProgressionGate(Giver& giver, uint32 scriptId)
        {
            if (!scriptId || !sConfig.progressionEnabled)
                return;

            std::string const& script = sObjectMgr->GetScriptName(scriptId);
            if (script == "npc_ipp_zg" || script == "npc_ipp_za")
            {
                giver.ipFrom = script == "npc_ipp_zg" ? sConfig.zulGurubState : sConfig.zulAmanState;
                return;
            }

            auto gate = std::find_if(PROGRESSION_GATES.begin(), PROGRESSION_GATES.end(),
                [&](ProgressionGate const& g) { return g.script == script; });
            if (gate == PROGRESSION_GATES.end())
                return;

            // hasPassedProgression() is never true for a state past the limit.
            uint8 limit = sConfig.progressionLimit;
            giver.ipFrom = limit && gate->from > limit ? IP_STATE_NEVER : gate->from;
            giver.ipBefore = gate->passedBefore && limit && gate->before > limit ? 0 : gate->before;
        }

        // WorldMapArea.dbc read whole: the core's own store is keyed by zone and drops the id the
        // client sends, and it skips the continent rows.
        bool LoadMapAreas(std::unordered_map<uint32, std::vector<uint32>>& byZone)
        {
            std::string path = sWorld->GetDataPath() + "dbc/WorldMapArea.dbc";
            DBCFileLoader dbc;
            // ID, MapID, AreaID, AreaName, LocLeft, LocRight, LocTop, LocBottom, DisplayMapID,
            // DefaultDungeonFloor, ParentWorldMapID
            if (!dbc.Load(path.c_str(), "niisffffiii") || dbc.GetCols() < 8)
            {
                LOG_ERROR("server.loading", "mod-quest-map: can't read {}; quest givers stay off the map", path);
                return false;
            }

            for (uint32 i = 0; i < dbc.GetNumRows(); ++i)
            {
                DBCFileLoader::Record row = dbc.getRecord(i);
                MapArea area;
                area.map = row.getUInt(1);
                area.zone = row.getUInt(2);
                area.left = row.getFloat(4);
                area.right = row.getFloat(5);
                area.top = row.getFloat(6);
                area.bottom = row.getFloat(7);
                // Continents and the world show no givers; a row without corners can't place one.
                if (!area.zone || area.left == area.right || area.top == area.bottom)
                    continue;
                uint32 id = row.getUInt(0);
                sAreas[id] = std::move(area);
                byZone[sAreas[id].zone].push_back(id);
            }
            return true;
        }

        void Build()
        {
            sAreas.clear();
            std::unordered_map<uint32, std::vector<uint32>> byZone;
            if (!LoadMapAreas(byZone))
                return;

            std::unordered_set<uint32> creatures;
            for (auto const& relation : *sObjectMgr->GetCreatureQuestRelationMap())
                creatures.insert(relation.first);
            std::unordered_set<uint32> objects;
            for (auto const& relation : *sObjectMgr->GetGOQuestRelationMap())
                objects.insert(relation.first);
            LoadEventGivers(creatures, "game_event_creature_quest");
            LoadEventGivers(objects, "game_event_gameobject_quest");

            std::unordered_map<uint32, uint32> savedCreatures;
            std::unordered_map<uint32, uint32> savedObjects;
            LoadSavedZones(savedCreatures, "creature");
            LoadSavedZones(savedObjects, "gameobject");
            std::unordered_map<uint32, std::vector<int16>> creatureEvents;
            std::unordered_map<uint32, std::vector<int16>> objectEvents;
            LoadEvents(creatureEvents, "game_event_creature");
            LoadEvents(objectEvents, "game_event_gameobject");

            WorldDatabaseTransaction trans = WorldDatabase.BeginTransaction();
            uint32 computed = 0;
            uint32 pins = 0;

            // entries: the giver entries the spawn can be, with their scripts.
            auto place = [&](SpawnData const& data, std::vector<std::pair<uint32, uint32>> const& entries, bool object,
                std::unordered_map<uint32, uint32> const& saved, std::unordered_map<uint32, std::vector<int16>> const& events,
                char const* table)
            {
                MapEntry const* map = sMapStore.LookupEntry(data.mapid);
                if (!map || map->Instanceable())
                    return;

                uint32 zone = 0;
                auto itr = saved.find(data.spawnId);
                if (itr != saved.end())
                    zone = itr->second;
                else
                {
                    uint32 area = 0;
                    sMapMgr->GetZoneAndAreaId(data.phaseMask, zone, area, data.mapid, data.posX, data.posY, data.posZ);
                    // A first start may load a lot of terrain here, before the world loop runs:
                    // keep the freeze detector (MaxCoreStuckTime) from taking it for a hang.
                    if (++computed % 200 == 0)
                        ++World::m_worldLoopCounter;
                    if (sConfig.saveZoneData && zone)
                        trans->Append("UPDATE {} SET zoneId = {}, areaId = {} WHERE guid = {}", table, zone, area, data.spawnId);
                }

                auto areas = byZone.find(zone);
                if (areas == byZone.end())
                    return;

                auto event = events.find(data.spawnId);
                for (uint32 id : areas->second)
                {
                    for (auto const& [entry, scriptId] : entries)
                    {
                        MapArea& area = sAreas[id];
                        if (area.map != data.mapid)
                            continue;

                        // World y runs right to left across a map, world x top to bottom.
                        float x = (data.posY - area.left) / (area.right - area.left);
                        float y = (data.posX - area.top) / (area.bottom - area.top);
                        if (!(x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f))
                            continue;

                        uint32 same = 0;
                        bool near = false;
                        for (Giver const& other : area.givers)
                        {
                            if (other.entry != entry || other.object != object)
                                continue;
                            ++same;
                            if (std::fabs(other.x / 10000.0f - x) < SAME_PLACE && std::fabs(other.y / 10000.0f - y) < SAME_PLACE
                                && other.phaseMask == data.phaseMask
                                && other.events == (event != events.end() ? event->second : std::vector<int16>()))
                                near = true;
                        }
                        if (near || same >= sConfig.maxPinsPerGiver)
                            continue;

                        Giver giver;
                        giver.entry = entry;
                        giver.object = object;
                        giver.map = data.mapid;
                        giver.zone = zone;
                        giver.x = uint16(std::lround(x * 10000.0f));
                        giver.y = uint16(std::lround(y * 10000.0f));
                        giver.phaseMask = data.phaseMask;
                        if (event != events.end())
                            giver.events = event->second;
                        SetProgressionGate(giver, scriptId);
                        area.givers.push_back(giver);
                        ++pins;
                    }
                }
            };

            std::vector<std::pair<uint32, uint32>> entries;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                // A creature_multispawn spawn is one of up to three creatures, picked when it
                // spawns: each that gives quests gets a pin.
                entries.clear();
                for (uint32 entry : { data.id, data.id2, data.id3 })
                {
                    if (!entry || !creatures.count(entry)
                        || std::any_of(entries.begin(), entries.end(), [&](auto const& e) { return e.first == entry; }))
                        continue;
                    if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry))
                        entries.emplace_back(entry, data.ScriptId ? data.ScriptId : creature->ScriptID);
                }
                if (!entries.empty())
                    place(data, entries, false, savedCreatures, creatureEvents, "creature");
            }

            for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
            {
                if (!objects.count(data.id))
                    continue;
                GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(data.id);
                if (!object)
                    continue;
                entries.assign(1, { data.id, data.ScriptId ? data.ScriptId : object->ScriptId });
                place(data, entries, true, savedObjects, objectEvents, "gameobject");
            }

            if (trans->GetSize())
                WorldDatabase.CommitTransaction(trans);

            LOG_INFO("server.loading", ">> mod-quest-map: {} quest giver pins on {} zone maps; worked out {} zones",
                pins, sAreas.size(), computed);
        }

        // ---- who sees what ---------------------------------------------------------------------

        // mod-individual-progression's state (0-18), or 18 when it isn't running or doesn't apply.
        uint8 ProgressionState(Player* player)
        {
            if (!sConfig.progressionEnabled || player->IsGameMaster())
                return IP_STATE_MAX;

            uint8 state = 0;
            for (uint8 i = 1; i <= IP_STATE_MAX; ++i)
                if (player->GetQuestStatus(IP_PROGRESSION_QUEST_BASE + i) == QUEST_STATUS_REWARDED)
                    state = i;
            return state;
        }

        struct Viewer
        {
            Player* player = nullptr;
            FactionTemplateEntry const* faction = nullptr;
            uint8 progression = IP_STATE_MAX;
            uint32 phaseMask = PHASEMASK_NORMAL;
            bool everything = false;
        };

        // The progression state that opens the continent or expansion a spawn is in; 0 = always open.
        uint8 RegionState(Giver const& giver)
        {
            if (giver.map == MAP_OUTLAND)
            {
                if (std::find(ZONES_OPEN_ON_OUTLAND_MAP.begin(), ZONES_OPEN_ON_OUTLAND_MAP.end(), giver.zone) != ZONES_OPEN_ON_OUTLAND_MAP.end())
                    return 0;
                return giver.zone == ZONE_ISLE_OF_QUEL_DANAS ? IP_STATE_QUEL_DANAS : IP_STATE_TBC;
            }

            MapEntry const* map = sMapStore.LookupEntry(giver.map);
            uint32 expansion = map ? map->Expansion() : 0;
            if (expansion >= 2)
                return IP_STATE_WOTLK;
            if (expansion == 1)
                return IP_STATE_TBC;
            return 0;
        }

        bool Shows(Viewer const& viewer, Giver const& giver)
        {
            if (!(giver.phaseMask & viewer.phaseMask))
                return false;
            if (!giver.events.empty())
            {
                // Out while any event it's listed under runs; gone while any it leaves for runs.
                bool any = false, needs = false;
                for (int16 event : giver.events)
                {
                    if (event < 0 && sGameEventMgr->IsActiveEvent(uint16(-event)))
                        return false;
                    if (event > 0)
                    {
                        needs = true;
                        any = any || sGameEventMgr->IsActiveEvent(uint16(event));
                    }
                }
                if (needs && !any)
                    return false;
            }
            if (viewer.everything)
                return true;
            if (sConfig.progressionEnabled
                && (viewer.progression < giver.ipFrom || (giver.ipBefore && viewer.progression >= giver.ipBefore)))
                return false;
            if (sConfig.hideLockedContinents && sConfig.progressionEnabled)
            {
                if (giver.map == MAP_EBON_HOLD)
                    return viewer.player->getClass() == CLASS_DEATH_KNIGHT;
                if (viewer.progression < RegionState(giver))
                    return false;
            }
            return true;
        }

        bool IsFriendly(Viewer const& viewer, uint32 factionTemplate)
        {
            if (viewer.everything || !viewer.faction)
                return true;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(factionTemplate);
            if (!faction)
                return true;
            if (faction->IsHostileTo(*viewer.faction))
                return false;
            return viewer.player->GetReputationRank(faction->faction) > REP_HOSTILE;
        }

        // A quest the giver shows a "!" for (Player::GetQuestDialogStatus), less the giver's own
        // script, which needs the creature in the world.
        bool Offers(Player* player, Quest const* quest)
        {
            if (player->GetQuestStatus(quest->GetQuestId()) != QUEST_STATUS_NONE)
                return false;
            ConditionList conditions = sConditionMgr->GetConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_QUEST_AVAILABLE, quest->GetQuestId());
            if (!sConditionMgr->IsObjectMeetToConditions(player, conditions))
                return false;
            return player->CanSeeStartQuest(quest) && player->SatisfyQuestLevel(quest, false);
        }

        uint32 QuestFlagsFor(Player* player, Quest const* quest)
        {
            uint32 flags = 0;
            // Weekly and monthly quests get the yellow "!" in the core.
            if (quest->IsRepeatable() && !quest->IsWeekly() && !quest->IsMonthly())
                flags |= QUEST_REPEATABLE;
            if (player->GetLevel() > uint32(player->GetQuestLevel(quest)) + sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF))
                flags |= QUEST_TRIVIAL;
            return flags;
        }

        // ---- requests --------------------------------------------------------------------------

        void HandleHello(Player* player, std::string const& req)
        {
            Send(player, Acore::StringFormat("HI:{}:{}:{}", req, PROTOCOL_VERSION, uint32(HELLO_GIVERS)));
        }

        void HandleArea(Player* player, std::string const& req, std::vector<std::string_view> const& args)
        {
            uint32 id = 0;
            if (args.size() < 3 || !ParseUInt(args[2], id))
            {
                SendError(player, req, "bad");
                return;
            }

            auto itr = sAreas.find(id);
            if (itr == sAreas.end())
            {
                Send(player, Acore::StringFormat("AR:{}:{}:0", req, id));
                Send(player, "AE:" + req);
                return;
            }
            MapArea const& area = itr->second;

            Viewer viewer;
            viewer.player = player;
            viewer.faction = player->GetFactionTemplateEntry();
            viewer.everything = player->IsGameMaster();
            viewer.progression = ProgressionState(player);
            // The player's phase is the one where they stand; anywhere else they'd see the normal one.
            viewer.phaseMask = player->GetZoneId() == area.zone ? player->GetPhaseMask() : PHASEMASK_NORMAL;

            int locale = player->GetSession()->GetSessionDbLocaleIndex();
            std::size_t const budget = MAX_PAYLOAD - std::string("AD:" + req).size() - 1;
            std::vector<std::string> rows;
            uint32 count = 0;
            for (Giver const& giver : area.givers)
            {
                if (count >= MAX_GIVERS || !Shows(viewer, giver))
                    continue;

                std::string name;
                QuestRelationBounds quests;
                if (giver.object)
                {
                    GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(giver.entry);
                    if (!object)
                        continue;
                    name = object->name;
                    if (GameObjectLocale const* names = sObjectMgr->GetGameObjectLocale(giver.entry))
                        ObjectMgr::GetLocaleString(names->Name, locale, name);
                    quests = sObjectMgr->GetGOQuestRelationBounds(giver.entry);
                }
                else
                {
                    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(giver.entry);
                    if (!creature || !IsFriendly(viewer, creature->faction))
                        continue;
                    name = creature->Name;
                    if (CreatureLocale const* names = sObjectMgr->GetCreatureLocale(giver.entry))
                        ObjectMgr::GetLocaleString(names->Name, locale, name);
                    quests = sObjectMgr->GetCreatureQuestRelationBounds(giver.entry);
                }

                std::vector<std::string> offered;
                for (auto quest = quests.first; quest != quests.second; ++quest)
                {
                    Quest const* info = sObjectMgr->GetQuestTemplate(quest->second);
                    if (!info || !Offers(player, info))
                        continue;
                    std::string title = info->GetTitle();
                    if (QuestLocale const* titles = sObjectMgr->GetQuestLocale(info->GetQuestId()))
                        ObjectMgr::GetLocaleString(titles->Title, locale, title);
                    offered.push_back(Fit(Acore::StringFormat("Q,{},{},{},{},", count, info->GetQuestId(),
                        player->GetQuestLevel(info), QuestFlagsFor(player, info)), title, budget));
                }
                if (offered.empty())
                    continue;

                rows.push_back(Fit(Acore::StringFormat("G,{},{},{},", count, giver.x, giver.y), name, budget));
                rows.insert(rows.end(), offered.begin(), offered.end());
                ++count;
            }

            Send(player, Acore::StringFormat("AR:{}:{}:{}", req, id, count));
            SendRows(player, "AD:" + req, rows);
            Send(player, "AE:" + req);
        }

        // ---- dispatch --------------------------------------------------------------------------

        void LoadConfig()
        {
            sConfig.enabled = sConfigMgr->GetOption<bool>("QuestMap.Enable", true);
            sConfig.saveZoneData = sConfigMgr->GetOption<bool>("QuestMap.SaveZoneData", true);
            sConfig.maxPinsPerGiver = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("QuestMap.MaxPinsPerGiver", 3), 1, 20);
            sConfig.hideLockedContinents = sConfigMgr->GetOption<bool>("QuestMap.HideLockedContinents", true);
            sConfig.progressionEnabled = sConfigMgr->GetOption<bool>("IndividualProgression.Enable", false, false);
            sConfig.progressionLimit = sConfigMgr->GetOption<uint8>("IndividualProgression.ProgressionLimit", 0, false);
            sConfig.zulGurubState = sConfigMgr->GetOption<uint8>("IndividualProgression.RequiredZulGurubProgression", 3, false);
            sConfig.zulAmanState = sConfigMgr->GetOption<uint8>("IndividualProgression.RequiredZulAmanProgression", 12, false);
        }

        bool TakeToken(SessionState& state)
        {
            uint32 now = getMSTime();
            if (state.lastRefill)
                state.tokens = std::min(REQUEST_BURST, state.tokens + getMSTimeDiff(state.lastRefill, now) * REQUEST_RATE / 1000.0f);
            state.lastRefill = now;
            if (state.tokens < 1.0f)
                return false;
            state.tokens -= 1.0f;
            return true;
        }

        std::vector<std::string_view> Split(std::string_view text, char sep)
        {
            std::vector<std::string_view> parts;
            std::size_t start = 0;
            while (true)
            {
                std::size_t pos = text.find(sep, start);
                if (pos == std::string_view::npos)
                {
                    parts.push_back(text.substr(start));
                    break;
                }
                parts.push_back(text.substr(start, pos - start));
                start = pos + 1;
            }
            return parts;
        }

        void Dispatch(Player* player, std::string_view message)
        {
            std::vector<std::string_view> args = Split(message, ':');
            if (args.size() < 2 || args.size() > 8)
                return;

            std::string_view command = args[0];
            std::string req(args[1].substr(0, 12));

            if (command == "HELLO")
                HandleHello(player, req);
            else if (command == "A")
            {
                if (TakeToken(sSessions[player->GetGUID().GetCounter()]))
                    HandleArea(player, req, args);
                else
                    SendError(player, req, "busy");
            }
            else
                SendError(player, req, "unknown");
        }
    }
}

using namespace QuestMap;

class QuestMapPlayerScript : public PlayerScript
{
public:
    QuestMapPlayerScript() : PlayerScript("QuestMapPlayerScript",
        { PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_ON_LOGOUT }) { }

    // The addon whispers itself; swallow those messages so they never show up as chat.
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (lang != LANG_ADDON || !receiver || receiver != player)
            return true;

        std::string const prefixTab = std::string(PREFIX) + "\t";
        if (msg.compare(0, prefixTab.size(), prefixTab) != 0)
            return true;

        if (!sConfig.enabled)
        {
            Send(player, "OFF:0");
            return false;
        }

        Dispatch(player, std::string_view(msg).substr(prefixTab.size()));
        return false;
    }

    void OnPlayerLogout(Player* player) override
    {
        sSessions.erase(player->GetGUID().GetCounter());
    }
};

class QuestMapWorldScript : public WorldScript
{
public:
    QuestMapWorldScript() : WorldScript("QuestMapWorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }

    // After every spawn and quest is loaded and before the maps update: the index reads zones
    // from the map files. Built even while disabled, so turning it on with a config reload works.
    void OnStartup() override
    {
        Build();
    }
};

void AddQuestMapScripts()
{
    new QuestMapPlayerScript();
    new QuestMapWorldScript();
}
