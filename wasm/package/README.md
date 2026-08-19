# @mundus-mirabilis/rpg-os

The **RPG OS** universal rule engine — a C++23 engine compiled to WebAssembly
so it runs anywhere JavaScript does.

> The part of a tabletop RPG that survives when you take the fantasy away:
> attributes and skills, dice rolls against thresholds, combat, hit points.
> RPG OS does that bookkeeping for **any** rule system — D&D, The Dark Eye,
> Basic Roleplaying, or one you wrote yourself — so you can focus on the
> wonderful world instead.

It is *universal* because it loads **any** ruleset JSON at runtime — the WASM
binary never embeds a game's data. All rule evaluation (character stats,
derived stats, checks, skill rolls, combat simulation, hit points) runs in the
compiled C++ engine.

Works in **Node.js** and in the **browser** (no server needed).

**Try it live:** the [ELO Arena](https://mundus-mirabilis.github.io/RPG-OS/main/demo/)
ranks every combatant of a ruleset in your browser and lets you enter your own
character. For the full picture — including the generated, strongly typed
headers and how to create your own ruleset — see the
[User Guide](https://mundus-mirabilis.github.io/RPG-OS/develop/user-guide.html).

## Install

```sh
npm install @mundus-mirabilis/rpg-os
```

## Versions

The package follows the project's versioning: **releases** are tagged
`vX.Y.Z`, the latest release lives on the `main` branch, and **development**
happens on `develop`. Each line is published to npm under its own dist-tag:

| npm dist-tag | Version | What it is |
| --- | --- | --- |
| `latest` | `X.Y.Z` | the latest release (`main`, tags `vX.Y.Z`) |
| `main` | `0.0.0-main.<sha>` | the latest `main` build |
| `develop` | `0.0.0-develop.<sha>` | the current development build |

```sh
npm install @mundus-mirabilis/rpg-os          # latest release
npm install @mundus-mirabilis/rpg-os@main     # latest main build
npm install @mundus-mirabilis/rpg-os@develop  # current development build
```

The docs and the ELO Arena are built for every version, so the latest release
and the current development state each have their own live links:

| Version | Documentation | ELO Arena |
| --- | --- | --- |
| Latest release (`main`) | [docs](https://mundus-mirabilis.github.io/RPG-OS/main/) | [ELO Arena](https://mundus-mirabilis.github.io/RPG-OS/main/demo/) |
| Current development (`develop`) | [docs](https://mundus-mirabilis.github.io/RPG-OS/develop/) | [ELO Arena](https://mundus-mirabilis.github.io/RPG-OS/develop/demo/) |

## Quick start — Node.js

```js
import { init } from '@mundus-mirabilis/rpg-os';
import { readFileSync } from 'node:fs';

const rpg = await init();
// Load the JSON with Node's native fs — nothing is baked into the WASM.
rpg.loadRulesetFromFile('node_modules/@mundus-mirabilis/rpg-os/rulesets/tde5e_core.json');

const geron = rpg.createEntity('geron');
console.log(geron.getStat('COU'));        // 12
console.log(geron.getResource('LP'));     // 31  (5 + 2*CON)
```

## Quick start — browser

```js
import { init } from '@mundus-mirabilis/rpg-os';

const rpg = await init();
const ruleset = await (await fetch('rulesets/dnd5e_srd.json')).text();
rpg.loadRuleset(ruleset);

const hero = rpg.createEntityFromSheet('my_hero', {
  STR: 16, DEX: 14, CON: 15, INT: 10, WIS: 12, CHA: 8,
  Attack: 5, AC: 17, Initiative: 2, HitPoints_Max: 30,
});
```

## API

| Method | Description |
| --- | --- |
| `await init()` | Loads the WASM module and returns an `RpgOs` instance. |
| `rpg.loadRuleset(json)` | Loads a ruleset from a JSON string (both runtimes). |
| `rpg.loadRulesetFromFile(path)` | Node-only: reads the JSON with `fs`. |
| `rpg.meta()` | Ruleset metadata (attributes, skills, pools) for building forms. |
| `rpg.entries()` | Named archetypes + bestiary entries in the ruleset. |
| `rpg.createEntity(id, opts?)` | Creates an entity from a named entry. |
| `rpg.createEntityFromSheet(id, stats)` | Creates an entity from **arbitrary** stats — any character, even one with no ruleset entry. |
| `entity.getStat/getResource/setStat/...` | Read/write stats and resource pools (LP/HP/AE/…). |
| `rpg.check(checkType, actor, target?, opts?)` | Resolves a named check. |
| `rpg.specFromId(id, weapon?)` / `rpg.specFromEntity(entity, weapon?)` | Build a combatant. |
| `rpg.fight(specA, specB, opts?)` | Runs one fight; loop it for Monte-Carlo win probabilities. |
| `rpg.fightDetail(specA, specB, opts?)` | Runs one fight and returns its full transcript — every round's dice rolls and hit-point changes, from the opening initiative roll to the final hit. Each cast action carries the spell's human-readable `spell_name`, its rolled `damage_dice`, and the spell-resource pool running down (`resource_before`/`resource_after`); `log.spells` lists each combatant's known spells and `log.resource_id`/`log.resource_pool` the starting spell resource. |
| `rpg.dispose()` | Releases the engine and all handles. |

## Rulesets

The package ships the project's rulesets under `rulesets/` as plain JSON data
(`dnd5e_srd.json`, `tde5e_core.json`, `brp_ugc.json`) for convenience. Because
they are loaded at runtime, you can pass **any** ruleset that follows the
[RPG OS ruleset schema](https://github.com/Mundus-Mirabilis/RPG-OS/blob/main/rulesets/ruleset.schema.json).

> **The shipped rulesets are NOT Apache-2.0.** Each ruleset carries **its own
> licence** in its JSON — read the `licence` field of the individual file
> before redistributing or modifying it. Every shipped ruleset also sets
> `licence_source`, a URL to where its rights holder states that licence, and
> the ORC-licensed ones carry the verbatim `licence_notice` (the ORC Notice)
> plus an `attribution` credit. `rpg.meta()` exposes `source`, `licence`,
> `licence_source`, `licence_notice` and `attribution` for the loaded ruleset
> so your UI can display them. Reproduce the `licence_notice`/`attribution`
> statements when you redistribute the content.

## License

Apache-2.0 — see [LICENSE](./LICENSE). This covers the **engine code and the
package itself only**; the ruleset JSON data under `rulesets/` has its own
individual licences (see above).

