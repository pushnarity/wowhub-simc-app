# WoW Hub healing sims (a modified SimulationCraft)

SimulationCraft is the best simulator World of Warcraft has, but it measures damage. Hand it a healer and it
either refuses (Holy Paladin, Mistweaver Monk) or sims the damage the healer would do in their spare time.
This fork teaches it to heal: a raid or a five-man group that takes damage the way real fights deal it, and
every healer spec played the way top players play it, checked spell by spell against their Warcraft Logs.

It is the engine behind the free sim tools on [wow.pushnarity.com](https://wow.pushnarity.com/en/retail/sims/)
(Quick Sim, Gear Check, Droptimizer and the BiS Planner), compiled to WebAssembly and run in the visitor's
browser. A healer pastes their character and gets healing per second, which gear, enchants and talents help
most, and what to farm next, the same way damage dealers always could.

- Branch **`wowhub-healer`** is the engine. SimulationCraft's `midnight` branch is merged into it every week,
  so everything SimulationCraft does (damage dealers, tanks, game data, items) stays current.
- Damage sims are SimulationCraft's own: nothing here changes a damage dealer's numbers.

## How close it comes to real players

Each spec was built against real kills from the top of the Warcraft Logs rankings. A *replay* puts the
logged healer's exact character (talents, gear, enchants, gems, stats at the pull) into the exact fight they
were logged in: every raid member takes the same damage at the same second, and the other healers heal what
they healed. Whatever is left to heal is our healer's job, and the sim's total is compared with theirs.

| Spec | Raid (Heroic Nek'zali kills) | Mythic+ (+22 to +24 keys) |
| --- | --- | --- |
| Restoration Druid | 92% | built on the raid build |
| Discipline Priest | 86-108% (four players) | 95% |
| Holy Priest | 99-109% (three players) | 93-99% (three keys) |
| Holy Paladin | 101-110% (three players) | 100-107% (four keys, Lightsmith) |
| Restoration Shaman | 98-108% (three players) | 95-106% (three keys) |
| Preservation Evoker | 99-102% (three players) | 94-96% (three keys) |
| Mistweaver Monk | 93-96% (three players) | 95-101% (three keys, fistweaving) |

100% means the sim healed exactly what the real player healed. The check also compares every spell's share
of the healing, not only the total.

We run these checks on our own server after every change. The logged fights and the players' characters
are Warcraft Logs' and their players' data, so they are not republished here; the engine needs none of it.
A replay file is plain text, so anyone with their own Warcraft Logs API access can make one (see
`heal_replay` below).

## What changed, piece by piece

### Healing targets that take real damage

Upstream SimulationCraft can add healing targets (`healing=<n>`), but nothing hurts them, so there is nothing
to heal. Here they take damage:

- **Generic fights.** `heal_damage_profile=raid` or `dungeon` (default `auto` picks by fight style). The raid
  profile is shaped from measured real Heroic pulls: about 4-5% of a raider's health per second, three
  quarters of the raid hit each second, tank hits, raid-wide pulses, spread damage and burst windows. The
  dungeon profile is shaped from real keys for a five-player group. Each damage profile is
  18 numbers (`heal_enemy_t::profile_t` in `engine/class_modules/sc_enemy.cpp`), and
  `heal_damage_profile=<18 numbers>` tries your own without a rebuild. `heal_damage_scale` makes any fight
  hit harder or softer.
- **Replays of real fights.** `heal_replay=<file>` plays back a logged fight half a second at a time: every
  raid member's maximum health and whether they tank, the damage each one took (absorbed damage included),
  what the other healers healed, the mana others gave (Innervate, Source of Magic) and the hits the healer
  took. Events are booked one at a time, so a 30-minute key fits.
  The file is plain text, one line each: `target <slot> <max health> <1 if a tank, else 0>` for every raid
  member, `e <seconds> <slot> <damage taken> <healing from others>` for each moment (slot `-1` is the healer
  themself), and `m <seconds> <mana>` for mana given. Run it with `healing=<number of targets>`,
  `max_time=<fight length>` and `vary_combat_length=0`.
- **Heals that behave like heals.** Smart heals pick the most injured allies; overhealing is counted apart
  and doesn't count as healing; shields count once they absorb damage; damage hits shields first; Leech heals
  only its owner.
- **New action list expressions** for healer priority lists: how many allies are below 95/90/80/60% health,
  the raid's missing health, and the group size.

### Every healer spec

Each spec has its own default healing priority list, written from what the top players in the logs cast and
how often. Where Mythic+ players take different talents, the list notices those talents and plays that
build instead. A healer is simmed as a healer when it has `role=heal` and healing targets; anything else
sims as before.

- **Restoration Druid.** Wildstalker blooms, Everbloom, treants, Efflorescence, the Season 2 tier set
  (Genesis on every Rejuvenation), Root Network, Reforestation's Tree of Life, Nurturing Dormancy, Dream of
  Cenarius, and mana from Innervate and Master Shapeshifter.
- **Discipline Priest.** Atonement with crit Atonement and Divine Aegis as the game logs them, Piety,
  Prayer of Mending with Assured Safety, Contrition, Twinsight, Unfolding Vision, Shadow Mend, Mythic+
  shielding, and a Flash Heal filler that paces itself on mana.
- **Holy Priest.** Benediction, Cosmic Ripple, Divine Hymn, Echo of Light (which never crits), Piety from
  periodic heals, Prayer of Mending that jumps to hurt allies, Renewed Faith, Lasting Words, and for Mythic+
  the Divine Image build with Epiphany, weaving damage between heals.
- **Holy Paladin.** Upstream refuses Holy Paladin, so this is a new healer
  (`engine/class_modules/paladin/sc_paladin_holy_heal.cpp`): Holy Shock and its echo, Infusion of Light,
  beacons (Beacon of Virtue, Beacon of the Savior), Eternal Flame, Dawnlight, Light of Dawn, the Season 2
  tier set, and for Mythic+ the Lightsmith build (Holy Armaments, Word of Glory, Holy Light, Shield of the
  Righteous, Greater Judgment, Hammer and Anvil, Divine Guidance).
- **Restoration Shaman.** Riptide, Chain Heal that waits for two injured allies, Healing Stream and
  Stormstream, Earthliving, Lively Totems, set bonuses, mana regeneration, and for Mythic+ the Healing Wave
  build (Ancestral Awakening, Healing Tide Totem, Earth Shield, Unleash Life, Spouting Spirits, Flow of the
  Tides, Quickstream). New file: `engine/class_modules/sc_shaman_resto_heal.inc`.
- **Preservation Evoker.** Echo on every ally, Dream Breath, Merithra's Blessing, Temporal Anomaly, Emerald
  Blossom, Living Flame, Stasis, Rewind when the raid is hurt, mana pacing, and for Mythic+ Verdant Embrace
  with Lifebind, Life-Giver's Flame, Lifespark, Flow State and Disintegrate with Energy Loop. New file:
  `engine/class_modules/sc_evoker_preservation_heal.inc`.
- **Mistweaver Monk.** Upstream has no Mistweaver healing, so this is new too
  (`engine/class_modules/monk/sc_monk_mistweaver_heal.inc`): Renewing Mist, Vivify, Enveloping Mist, Rushing
  Wind Kick, Misty Coalescence and Mistline, Yu'lon, Spiritfont, Life Cocoon, Revival, and for Mythic+ the
  Chi-Ji fistweaving build (Ancient Teachings, Way of the Crane, Crane Style, Sheilun's Gift, Invoke Chi-Ji).

### Smaller fixes that help everyone

- Mistweaver takes Intellect from Agility/Intellect gear; a real paste had about half its Intellect before.
- Holy Paladin and Restoration Shaman get the current season's flask, food, potion and weapon oil by
  default; they still had Shadowlands ones.

## Known limits

We'd rather say it than have you find it:

- A few spells still sit 10-30% off the logs on their own, even where the total matches: Leech reads about
  1.3-1.5 times too high for some specs, the Soulcoiler trinket heals less than in some logs, and Divine
  Hymn lands on a hurt raid more often than real players' does.
- The Preternatural Antivenom and Felheart trinkets are approximated, not built.
- Some casts real healers make rarely are not in: Holy Priest's Sanctify, Prayer of Healing, Heal and
  Guardian Spirit casts; Holy Paladin's Lay on Hands and Brought to Light.
- The replay profiles raise a few players' stats to their fight averages, because other raiders' buffs aren't
  all simmed. A pasted character gets no such raise, so it sims what its own gear gives.

## Building

This branch builds like plain SimulationCraft (cmake; see
[their build guide](https://github.com/simulationcraft/simc/wiki/HowToBuild)). The browser build the site ships has no threads, plus one small change to
`engine/sim/profileset.cpp` (it reads every profileset before the sim starts, since a browser build can't
start the profileset thread). The site's [engine page](https://wow.pushnarity.com/en/retail/sims/engine/) shows
that change and offers the complete source of every build it ships.

## Based on SimulationCraft

All the hard work under the hood is [SimulationCraft](https://github.com/simulationcraft/simc)'s: years of
class modules, game data and tooling by its developers and contributors. This fork only adds healing on top.

- For SimulationCraft itself (downloads, the starter guide, its wiki, its Discord) and for bugs in damage sims,
  go to [their project](https://github.com/simulationcraft/simc). Bugs in healing sims are ours, not theirs:
  please don't report them to SimulationCraft.
- Please use the full name SimulationCraft (or SimC) for their project; SimCraft is a different thing.
- This fork is not made or endorsed by the SimulationCraft team.

## Modification notice

This is a modified version of SimulationCraft (https://github.com/simulationcraft/simc), changed by Pushnarity
(WoW Hub) from November 2026 on. The date and content of every change are in this branch's git history
(`git log midnight..wowhub-healer`).

- Added files (each carries a notice at its top): `engine/class_modules/paladin/sc_paladin_holy_heal.cpp`,
  `engine/class_modules/sc_shaman_resto_heal.inc`, `engine/class_modules/sc_evoker_preservation_heal.inc`,
  `engine/class_modules/monk/sc_monk_mistweaver_heal.inc`.
- Modified files (our changes are marked `// wowhub` where they are not obvious): `engine/action/heal.cpp`,
  `engine/action/heal.hpp`, `engine/class_modules/sc_enemy.cpp`, `engine/class_modules/sc_druid.cpp`,
  `engine/class_modules/sc_evoker.cpp`, `engine/class_modules/sc_shaman.cpp`,
  `engine/class_modules/priest/sc_priest.cpp`, `engine/class_modules/priest/sc_priest.hpp`,
  `engine/class_modules/priest/sc_priest_discipline.cpp`, `engine/class_modules/priest/sc_priest_holy.cpp`,
  `engine/class_modules/paladin/sc_paladin.cpp`, `engine/class_modules/paladin/sc_paladin.hpp`,
  `engine/class_modules/monk/sc_monk.cpp`, `engine/class_modules/monk/sc_monk.hpp`,
  `engine/class_modules/apl/apl_monk.cpp`, `engine/player/player.cpp`, `engine/sim/sim.cpp`,
  `engine/sim/sim.hpp` and `source_files/cmake_engine.txt`. This README is ours too: SimulationCraft's own is at
  https://github.com/simulationcraft/simc, and its notices for external libraries are kept word for word below.

## Licence

GPL-3.0, like SimulationCraft (see `LICENSE`). Made by [Pushnarity](https://wow.pushnarity.com); not
affiliated with the SimulationCraft team. World of Warcraft is a trademark of Blizzard Entertainment, Inc.;
this project is not affiliated with or endorsed by Blizzard.

## External Libraries

This program uses the following external libraries.

RapidJSON (http://rapidjson.org)

Copyright (c) 2015 THL A29 Limited, a Tencent company, and Milo Yip. All rights reserved.
MIT License (see LICENSE.MIT for more information).

RapidXML (http://rapidxml.sourceforge.net/index.htm)

Copyright (c) 2006, 2007 Marcin Kalicinski. All rights reserved.
MIT License (see LICENSE.MIT for more information).

The MSInttypes r29 (https://code.google.com/p/msinttypes/)

Copyright (c) Alexander Chemeris. All rights reserved.
BSD 3-Clause License (see LICENSE.BSD for more information).

The Qt Toolkit (https://www.qt.io/)

Copyright (c) 2016 The Qt Company Ltd. and other contributors. All rights reserved.
GNU Lesser General Public License, version 3 (see LICENSE.LGPL for more information).

UTF-8 CPP (https://github.com/nemtrif/utfcpp)

Copyright (c) 2006 Nemanja Trifunovic. All rights reserved.
Boost Software License, Version 1.0 (see LICENSE.BOOST for more information).

{fmt} (https://github.com/fmtlib/fmt)

Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors. All rights reserved.
MIT License (see LICENSE.MIT for more information).

cpp-semver (https://github.com/easz/cpp-semver)

Copyright (c) 2018 Cas Perl. All rights reserved.
MIT License (see LICENSE.MIT for more information).

utf8.h (https://github.com/sheredom/utf8.h)

Unlicense License (see LICENSE.UNLICENSE for more information).
