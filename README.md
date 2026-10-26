# Quest Map

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module that puts quest givers on the
world map, the way retail does.

The 3.3.5 client only knows the creatures the server has sent it, the ones within visibility range
(100 yards on a continent), so a quest giver's "!" turns up on the minimap only once you're nearly
there. With this module, a map addon asks the server which quest givers are on the zone map it
shows, and the server answers with the ones whose quests you can take right now.

- **The core's own rules.** A giver is listed for a quest when the core would show its "!": the
  quest isn't in your log or done, and you meet its level, class, race, skill, reputation,
  prerequisite quests, conditions and daily/weekly limits (`Player::CanSeeStartQuest` and
  `SatisfyQuestLevel`, as in `Player::GetQuestDialogStatus`).
- **Creatures and objects.** Quest-starting objects (wanted posters, books) count too.
- **What you can see.** Spawns from game events only while the event runs (holiday quests and
  their givers included); phasing on the map you stand in; hostile factions left out. A
  `creature_multispawn` spawn gets a pin for each of its creatures that gives quests.
- **mod-individual-progression.** Givers that IP's visibility scripts (`npc_ipp_*`, `gobject_ipp_*`)
  hide at your progression are left out, and so is Outland before TBC and Northrend before WotLK.
  Quests that IP gates by condition are covered by the conditions check.
- **Repeatable and trivial quests** are flagged, so the addon can draw a blue "!" or hide the grey
  low-level ones (`Quests.LowLevelHideDiff`).

The map addon is [our DragonUI fork](https://github.com/buildthehomelab/wow-addon-DragonUI): its
world map draws the pins, lists the quests in the pin's tooltip and has "Show Quest Givers" and
"Show Trivial Quests" in its filter menu. Without this module DragonUI simply shows no quest givers.

## Install

```bash
cd azerothcore/modules
git clone https://github.com/buildthehomelab/wow-mod-quest-map.git mod-quest-map
```

Clone it into `mod-quest-map` exactly: AzerothCore derives the loader symbol
(`Addmod_quest_mapScripts`) from the folder name. Re-run CMake, build, and copy
`conf/mod_quest_map.conf.dist` to `mod_quest_map.conf` next to `worldserver.conf`.

There is no SQL to apply. On the first start the module works out the zone of every quest giver
spawn without one from the map files and, with `QuestMap.SaveZoneData = 1`, saves it in
`creature.zoneId` / `gameobject.zoneId`, so that start takes a little longer.

## Config

| Option | Default | |
|---|---|---|
| `QuestMap.Enable` | 1 | Master switch. |
| `QuestMap.SaveZoneData` | 1 | Save worked-out spawn zones for later starts. |
| `QuestMap.MaxPinsPerGiver` | 3 | Pins one giver gets on one zone map at most. |
| `QuestMap.HideLockedContinents` | 1 | With IP: no givers on continents not open yet. |

## Protocol

Addon messages with the prefix `QMP`, whispered by the client to itself and answered the same way.
Fields are `:`-separated, rows `;`, values `,`; names escape `,;:|%` as `%XX`.

```
HELLO:<req>:<protocol>   -> HI:<req>:<version>:<flags>
A:<req>:<map area>       -> AR:<req>:<map area>:<givers>, AD:<req>:<rows>..., AE:<req>
    G,<n>,<x>,<y>,<name>                    a quest giver; x, y in 1/10000 of the map
    Q,<n>,<quest>,<level>,<flags>,<title>   one of giver n's quests; flags 1 repeatable, 2 trivial
```

`<map area>` is the WorldMapArea.dbc id, `GetCurrentMapAreaID() - 1` on the client. Continent
and world maps answer with no givers. Each character may ask about 1 map a second (bursts of 8);
past that the answer is `ERR:<req>:busy`.

## License

MIT
