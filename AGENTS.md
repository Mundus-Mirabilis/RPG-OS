# AGENTS.md

Instructions for LLM coding agents (and human contributors) working in this
repository. **Read and follow these rules for every change you make.**

## Project

RPG OS — a **C++23, header-only library**: a universal rule engine and
"operating system" for tabletop role-playing games. All rules live in
ruleset-specific JSON files; the engine resolves them in two modes:

- **Universal mode (dynamic):** `rpg_os::RulesetEngine` loads *any* ruleset
  JSON at runtime and resolves rules generically (AST formula evaluator,
  generic check resolver, modifier pipeline, cost tables, event system).
  Adding a new game system requires only a new JSON file — no recompilation.
- **Specific mode (codegen):** `codegen/rpg_os_codegen.py` compiles a ruleset's
  *schema* into a strongly typed header (`generated/<ruleset>.hpp`) with named
  members and methods (`courage()`, `baseAttack()`, `climbCheck()`, ...).
  Formulas are compiled to C++ and checks instantiate the shared templates
  with `constexpr` configs (zero dynamic allocation in the hot path). The
  generated code **still loads the JSON at runtime** for the *data* database
  (creatures, items, archetypes).

Both modes share one header-only, template-based core
(`include/rpg_os/core/`, `include/rpg_os/common/`).

## Ground rules

0. **Never commit on the user's behalf.** The human always reviews and commits
   their own work (`commit.gpgsign=true`; a human quality check is required
   before any commit). Leave changes staged or in the working tree and hand
   the commit to the user with a clear summary of what is staged.
1. **Test-driven development (TDD) is mandatory.** Write a failing test first
   (red), implement the minimal code to pass (green), then refactor. Never add
   functionality without a test that exercises it. Keep the test suite green
   before finishing a change.
2. **Style is enforced — do not deviate.** Run `clang-format -i <file>` on
   every file you touch (config: `.clang-format`).
   - Indentation (shift width): **2 spaces**
   - Tab width: **8 columns**; use **spaces only**, never tab characters
   - Column limit: **100**
3. **Keep clang-tidy clean.** `.clang-tidy` enables `modernize-*`,
   `cppcoreguidelines-*`, `performance-*`, etc. Do not introduce new warnings;
   fix them before finishing. Never run the formatter/linter on vendored code
   under `include/rpg_os/third_party/`.
4. **Modern C++23 only.** Prefer:
   - `const` / `constexpr` where possible, `noexcept` where appropriate
   - value semantics and smart pointers — no manual `new`/`delete`
   - structured bindings, `std::ranges`, concepts where they improve clarity
   - `std::array`/`std::vector` over C arrays, `static_cast`/`reinterpret_cast`
     over C-style casts
   - `std::string_view`/`std::span` for non-owning parameters
   - templates / template metaprogramming to share algorithms between the
     universal and specific modes (see "Shared template core")
5. **Header-only first.** The library is header-only; keep it that way. Prefer
   `inline`/header-local state over out-of-line `.cpp` files. Reserve `src/`
   for a single documented exception if one ever becomes unavoidable.
6. **Don't break the build.** Configure, build, and test before finishing
   (commands below).

## Build & test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Useful options:
- `-DRPG_OS_WARNINGS_AS_ERRORS=ON` — treat warnings as errors (enforced in CI)
- `-DRPG_OS_ENABLE_SANITIZERS=ON` — ASan + UBSan
- `-DRPG_OS_BUILD_TESTS=OFF` — skip the test suite
- `-DRPG_OS_RUN_CODEGEN=ON` — regenerate the codegen outputs during the build

Codegen (manual):

```sh
python3 codegen/rpg_os_codegen.py --ruleset rulesets/dnd5e_srd.json --out generated/
python3 codegen/rpg_os_codegen.py --ruleset rulesets/tde5e_core.json --out generated/
```

Generated headers are committed; keep them in sync with the rulesets. A CI
step regenerates them into a temp dir and checks `git diff --exit-code`.

## Project layout

| Path | Purpose |
| --- | --- |
| `include/rpg_os/common/` | Shared value types, JSON alias, event system |
| `include/rpg_os/core/` | Shared header-only template core (concepts, dice, math, modifier, cost tables, checks, entity) |
| `include/rpg_os/universal/` | Universal (dynamic) mode: ruleset loader, AST evaluator, dynamic entity, check resolver, engine facade |
| `include/rpg_os/specific/` | Shared runtime support used by generated code (`sheet.hpp`, a CRTP base implementing the common data-database loaders) |
| `include/rpg_os/third_party/` | Vendored headers (`nlohmann/json.hpp`, `doctest/`) — never edit, never format |
| `src/` | Reserved for a future out-of-line TU; the library is header-only today |
| `codegen/` | `rpg_os_codegen.py` + `validate_ruleset.py` — Python 3 (stdlib only) code generator and schema validator |
| `rulesets/` | Ruleset JSON files (`dnd5e_srd.json`, `tde5e_core.json`) and `ruleset.schema.json` |
| `generated/` | Committed codegen outputs (`dnd5e_srd_static.hpp`, `tde5e_core_static.hpp`) |
| `tests/` | doctest suite, registered with CTest via `tests/CMakeLists.txt` |
| `examples/` | Small self-contained demo programs |
| `scripts/` | Python tooling — Monte-Carlo ELO ranking (`elo_ranking.py`) |
| `cmake/` | CMake helper modules (warnings, sanitizers) |
| `wasm/` | WebAssembly port: extern "C" binding (`bindings.cpp`), build script, and the `@mundus-mirabilis/rpg-os` npm package source |
| `web/` | Static, no-build web demo (`web/demo/` = the ELO Arena page) |

The `rpg_os` CMake target is an **INTERFACE (header-only)** target; do not
convert it to `STATIC`. Only tests and examples compile executables.

### WebAssembly / npm

- `wasm/bindings.cpp` is a flat extern "C" API over the universal engine,
  compiled to WASM by `wasm/build.sh` (em++). It stays host-compilable (no
  Emscripten headers unless `__EMSCRIPTEN__`) so `g++ -std=c++23 -fsyntax-only`
  works as a local syntax check.
- The npm package `@mundus-mirabilis/rpg-os` lives in `wasm/package/` and is
  assembled by `wasm/build.sh` (WASM dist + rulesets + LICENSE). The ELO
  ratings deliberately stay in Python (`scripts/elo_ranking.py`); the demo
  ports only the small formula to JS. CI: `wasm.yml` (build+test), `npm-publish.yml`
  (publishes on every main/develop push under the `main`/`develop` dist-tags and
  on `v*` tags under `latest`), and the `deploy-demo` job of `docs.yml` deploys
  `web/demo/` to `/<version>/demo/` AFTER the docs (chained via `needs`, so the
  two gh-pages pushes can never race).
- The `wasm/` build output and `web/demo/{wasm,rulesets,data}` are CI-generated
  and gitignored (see `.gitignore`).
- **Portable seeded RNG**: `DefaultRandom::operator()` draws from the raw
  `std::mt19937` output, NOT `std::uniform_int_distribution` (which differs
  between libstdc++ and libc++). This makes "same seed = same result" hold
  across native builds and the WASM build (libc++), which the npm package's
  native-vs-WASM fight parity test relies on (pinned by `test_dice.cpp`). Do
  not "restore" the distribution — it would break WASM↔native parity.
- **Arbitrary characters can fight**: `CombatantSpec` carries an optional
  embedded `Json sheet`; `createFighter` builds the fighter from it, and
  `makeCombatantSpecFromEntity(engine, entity, weapon, out)` derives combat
  values from ANY `DynamicEntity` — a character with no ruleset entry (e.g.
  one entered into a form) is fully supported. `DynamicEntity::refreshResources`
  is public so hand-built sheets get their resource pools. **The weapon a
  character actually has equipped is the one it fights with**: the spec
  builders resolve the equipped item's `damage` expression (an explicit
  caller-provided weapon string overrides; the default longsword is only a
  fallback for weapon-less characters).

## Shared template core

The engine is one header-only template layer instantiated differently by each
mode:

- `core/concepts.hpp` defines `template<typename T> concept StatProvider`
  requiring `t.getStat(std::string_view) -> std::integral` — any integral
  storage width. Universal entities use a hash map of `int32_t`; generated
  characters store attributes/skills in the narrowest type that fits the
  ruleset bounds (`uint8_t` for the shipped rulesets) and satisfy the concept
  via a string→member switch (no allocations). Named getters are convenience
  API on top.
- `core/checks.hpp` is **ruleset-agnostic by design**: a check is a generic,
  data-driven `CheckRecipe` (resolution threshold/pool/opposed/resistance,
  dice, comparison, reference source, critical style, grading, difficulty
  handling) resolved by the single `resolveCheck` template. There is no
  `AdditiveD20`/`RollUnderD20`-style named mechanism anywhere in the core — a
  new ruleset, including one with a new combination of dice/comparison/
  grading, is expressed purely as recipe JSON. Universal mode interprets a
  runtime recipe; generated code builds a recipe from literals and calls the
  same `resolveCheck`. The three percentile variants are all configurations of
  the threshold/opposed/resistance resolutions (graded
  Critical/Special/Success/Fumble via `CheckResult::successLevel`, opposed
  level matrix, Resistance Table).
- Shared helpers: `core/dice_engine.hpp` (injectable RNG), `core/math.hpp`
  (floor/ceil/round/clamp used by the AST evaluator *and* generated code),
  `core/modifier.hpp` (base→override→add→multiply→clamp pipeline),
  `core/cost_table.hpp` (threshold + multiplier), `core/variance.hpp`
  (range selection: weakest / weak / average / strong / strongest / random
  via `rpg_os::readVariantValue`).
- A **parity test** asserts universal and specific modes produce identical
  results for the same scenario.

## Ruleset JSON conventions

- One file per system: `rulesets/<ruleset_id>.json`, containing the *schema*
  (rules) and a `data` section (creatures, items, archetypes, spells,
  conditions, poisons, diseases).
- **Licensing:** ruleset JSONs are **not** Apache-2.0 — each is governed by
  the licence in its own `licence` field (never assume Apache-2.0 for rules).
  Every ruleset is validated against `rulesets/ruleset.schema.json` (JSON
  Schema draft-07). `licence` is **required** (non-empty); `licence_source`
  (optional http(s) URL pointing to where the rights holder states the
  licence — set it for every shipped ruleset), `licence_notice` (optional
  verbatim notice the licence requires — e.g. the ORC Notice for
  ORC-licensed rulesets), `attribution` (optional credit/attribution the
  licence requires — e.g. the CC-BY-4.0 or ORC attribution; set it for every
  shipped ruleset) and `comment` (optional) may follow. Run
  `python3 codegen/validate_ruleset.py rulesets/*.json` after editing any
  ruleset; CI enforces it.
- Top level: `schema_version`, `ruleset_id`, `ruleset_name`, `source`,
  `licence` (required), `licence_source` (optional), `licence_notice`
  (optional), `attribution` (optional), `comment` (optional),
  `namespace` (used by codegen),
  `spell_resource` (optional: the resource pool spell casting draws its cost
  from; enables `RulesetEngine::castSpell`), then `attributes`,
  `derived_stats`, `resource_pools`, `skills`, `check_types`, `cost_tables`,
  `equipment_slots`, `event_triggers`, `data`. **Optional bookkeeping
  sections** (each is opt-in — a ruleset without one simply does not use that
  feature): `currencies` (denominations with `per_base` values in the
  `base_unit`; enables `Money`/`itemPrice`/`buy`), `encumbrance` (up to three
  independent capacity axes as stat formulas — `capacity` weight, `size_capacity`
  volume/slots, `item_capacity` count — plus ratio `levels` with an optional
  condition per level; the engine reports what a sheet carries via
  `carriedLoad`, its limits via `capacity`, the per-axis status via
  `inventoryStatus`, and whether an item fits via `canCarry`; an item record
  may declare a numeric `size` and, for containers, a `capacity`
  `{"weight","size","items"}` that `addItemToContainer` enforces with
  `OverCapacity`), `spellcasting` (`style` `pool`/`slots`; vancian per-day
  `slots` per spell level), and `movement` (named movement modes with a base
  speed formula, named terrains that multiply per-mode speed / forbid a mode
  or require a capability / change exhaustion / set `regeneration`
  `normal`/`none`/`half`, an optional exhaustion `pool` drained by `move`, and
  `load` ratio→factor steps that slow a loaded carrier — see `movementStatus`,
  `movementSpeed`, `canRegenerate`, `move`; a ruleset without `movement` is
  unconstrained). The bookkeeping layer (inventory, equipment, conditions with
  durations, advancement, rests, curses, movement) is universal and
  data-driven;
  `data.conditions` may carry `stat_modifiers` (per-stack stat changes) and
  `check_modifiers` (per-check flat bonuses, advantage/disadvantage,
  auto-failure, and/or bonus dice, matched by scope — a check type id, a
  wildcard prefix, a category keyword `attack`/`save`/`check`/`skill`, or
  `all` — and by side: the carrier's own checks or checks *against* the
  carrier), and `data.curses` is an
  `afflictionRecord`-shaped section, so everything a rulebook covers is
  modelable without engine changes. Conditions and traits may also carry
  `restrictions` (what the carrier *cannot* do — `no_action`,
  `no_bonus_action`, `no_reaction`, `no_move`, `no_speak`, `no_concentration`,
  `no_cast`; queried via `DynamicEntity::hasRestriction`/`restrictions` and
  *enforced* by `RulesetEngine::actionAllowed` in `castSpell` and the combat
  helpers, which refuse the action before any resource is spent) and
  `capabilities` (what it *can* do — movement/senses like `swim`, `climb`,
  `breath_water`, `darkvision`, `see_invisible`; queried via
  `DynamicEntity::hasCapability`/`capabilities`). The surrounding can affect
  items too: a sheet has a current `terrain` (`DynamicEntity::setTerrain`),
  and an item record may carry a `terrain` object keyed by terrain id with
  `unusable`/`ruined` (reported per item via `itemTerrainStatus`) and `grants`
  — capabilities the *wearer* gains automatically in that terrain (an amulet
  that grants `breath_water` in water), which is what makes a terrain's
  movement-mode `requires` requirement satisfiable. Casting requires the
  standard action, so `no_action` also blocks `cast` (an explicit `no_cast`
  is reported as the reason when present). The shipped D&D conditions carry
  these restrictions (incapacitated blocks action/bonus/reaction; paralyzed,
  petrified, unconscious also block move/speak/concentration; restrained and
  grappled block move). The engine never makes a
  caller-side choice for the caller: a spell effect of `kind` `options`
  declares an option group (`id` + `options` array of effect records, each
  recursively resolvable including its own save/attack/ongoing logic). A
  required group without a selection is reported on the result's
  `choicesRequired` (nothing applied) so the caller knows a choice is pending;
  an `optional` group is skipped silently; with a selection (a map of group id
  → option index passed to `castSpell`/`resolveEffects`) the chosen option
  resolves. Inherent, always-on creature abilities are
  `data.traits`: trait definitions (same `stat_modifiers`/`check_modifiers`
  vocabulary as conditions, plus an `effects` array resolved once when a
  creature carrying the trait is created — resistances, recurring
  regeneration) referenced by id from a creature's `traits` array,
  applied to every check the creature makes or is the target of. Every
  creature trait in the shipped rulesets is a trait id (no prose strings left
  in creature `traits` arrays): movement/senses become `capabilities`,
  unconditional save/attack edges become `check_modifiers`, regeneration and
  always-on conditions become `effects`, and anything the engine cannot
  enforce keeps its full prose as the trait's `description` — the engine
  exposes `hasTrait`/`traits()` so a caller takes note of the rule while the
  engine enforces what it can. Spells carry
  a machine-readable `effects`
  array (`effectRecord` in the schema: damage/condition/heal/temp_hp/resist/
  stat_bonus, each optionally gated by a `save` or an `attack`; `stat_bonus`
  applies a temporary `add` to the target's effective `stat` for `duration`
  ticks; `bonus_die` grants a temporary die (`dice` + `scope`) rolled and
  added to matching checks — D&D's Bless; any effect may carry an `ongoing`
  object (`at` `start_of_turn`/`
  end_of_turn`, `duration`, optional nested `effect`) to re-apply itself on
  the target's turn — per-round poison damage, regeneration, an arrow's
  4d4-now-2d4-later). Effect dice, `add`
  amounts, condition `stacks`, and roll-under save `dc`s are formulas that may
  reference the casting check's quality level as `env.ql` (The Dark Eye's
  QL-scaled spells), and `saveDef.comparison` `"le"` models roll-under
  resistance (stat − QL) versus the D&D-style roll-over DC. A prose rule that
  is extracted into these structured fields is removed from the entry's
  `description`, which the engine never reads. The D&D ruleset's `data`
  section carries only the machine-readable sections the engine consumes
  (`creatures`, `spells`, `items`, `conditions`, `traits`, `poisons`); the
  `weapons` / `armor` arrays are optional classification sub-views of
  `data.items` (the engine reads gear from `items`), so keep them in sync;
  raw SRD reference text (rulebook chapters, class
  features, magic items, glossary, species, backgrounds, feats) is *not*
  embedded — it lives in the source `.local_ressources/DnD/` extraction and
  is modeled as structured rules when the engine gains a mechanism for it.
- Attribute ids are short uppercase codes (`COU`, `STR`); `name` is the human
  name used to derive C++ identifiers (`"Courage"` → `courage`).
- Derived-stat `formula` strings use the restricted grammar: arithmetic
  (`+ - * / % ^`), comparisons, `&& || !`, functions `min max floor ceil
  round clamp`, and identifiers (attribute ids, `actor.*`, `target.*`,
  `env.*`, parameters). The grammar must be translatable to C++ by codegen.
- `check_types` entries are named, data-driven `CheckRecipe`s (e.g.
  `dnd5e_attack_melee`, `tde_attack`) described entirely by generic fields:
  `resolution` (`threshold`/`pool`/`opposed`/`resistance`), `dice`,
  `comparison` (`ge`/`le`), `threshold_source`, `bonus_stats`,
  `pool_attributes`/`pool_stat`, `attack_stat`/`parry_stat`/`compare_levels`,
  `critical_style`/`critical_face`/`critical_confirm` and `fumble_*`,
  `grading`, `difficulty_mode`, `difficulty_multiplier`. The universal
  resolver interprets them; codegen emits named methods that build a
  `static const rpg_os::CheckRecipe` and call `rpg_os::resolveCheck`.
- **Ranges:** a numeric data value may be a plain integer, a dice expression
  string (`"2d6+4"`, kept verbatim from the source), or a range object
  `{"min": …, "max": …}`. Never collapse a source range into a single number.
  Range/stat values are resolved per `rpg_os::Variance` (weakest / weak /
  average / strong / strongest / random) via `rpg_os::readVariantValue`
  (`include/rpg_os/core/variance.hpp`).
- **Item records** (`data.items` — weapons, armor, containers, magic items)
  are the engine's gear source. A **weapon** declares `damage` as a *bare
  dice expression* (`"1d8"` — **never** `"1d8 Bludgeoning"`; the damage
  type lives in its own `damage_type` field), a `slot` (`weapon_hand`), a
  `properties` array of keywords (Finesse / Light / Reach / Two-Handed /
  Thrown / Ammunition / ...), an optional `range` (`{"normal","long"}`
  feet), `versatile_damage`, `ammunition`, and a `mastery` keyword. An
  **armor** item declares a `slot` (`body_armor` / `shield`), a `modifiers`
  array carrying its AC effect, a numeric `strength_req`, and
  `stealth_disadvantage`. Equipped-item `modifiers` apply to the wearer's
  effective stats (`DynamicEntity::getEffectiveStat`) and a modifier's
  `value`/`factor`/clamp bounds may be a **formula string evaluated against
  the wearer** — e.g. D&D medium armour caps its Dexterity contribution with
  `"10 + min(DEX_mod, 2) + 4"`. `weight` (`"2 lb."`) and `cost` (`"2 SP"`)
  are kept verbatim from the source; the engine parses them at runtime
  (`parseWeightValue` / `parsePriceString`). Containers declare a `capacity`
  object `{"weight","size","items"}` that `addItemToContainer` enforces.
  **Combat uses the weapon a combatant actually has equipped**: the spec
  builders resolve the equipped item's `damage`, and `validate_ruleset.py`
  flags a `damage` string that embeds its damage type — the exact regression
  that would silently make a weapon's real damage unused in fights.
- Adding a system = adding a JSON file; do not change engine code for it.

## Code conventions

- **Licence header:** every code file (headers, sources, Python scripts,
  CMake files) starts with
  `// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.`
  plus `// SPDX-License-Identifier: Apache-2.0` (use `#` for Python/CMake).
  Generated headers emit it automatically via the codegen banner.
- **Naming:** use `rpg_os` (never `rpgos`) wherever the project's name cannot
  be written as "RPG OS" — the C++ namespace is `rpg_os`, include paths are
  `<rpg_os/...>`, and CMake identifiers/options are `RPG_OS_*`.
- Header guards: `#pragma once`.
- Namespaces: everything under `namespace rpg_os`; **no** `using namespace` in
  headers.
- Naming (LLVM-flavoured, matching `.clang-format`):
  - Types / classes / templates: `PascalCase` (`RuleEngine`)
  - Functions / variables: `camelCase` (`evaluateRule`, `activeRules`)
  - Constants / macros: `SCREAMING_SNAKE_CASE`
  - Member variables: `m_camelCase` (`m_engine`)
- Public API in headers gets Doxygen comments (`///` or `/** */`) describing
  purpose, parameters, and return value.
- Error handling: exceptions or `std::expected` (C++23); avoid raw error codes
  where a type is clearer.
- Keep functions short and focused; mind `-Wconversion` / `-Wsign-conversion`.
- Generated headers: `// AUTOMATICALLY GENERATED BY RPG OS CODEGEN — DO NOT
  HAND EDIT`, produced by `codegen/rpg_os_codegen.py`, clang-format clean.

## Definition of done

- [ ] Test written first (TDD red) and passing (green) for the change
- [ ] `clang-format -i` applied to all touched files (not `third_party/`)
- [ ] No new clang-tidy or compiler warnings
- [ ] `cmake --build build` succeeds
- [ ] `ctest` passes — the full suite including parity and codegen-output tests
- [ ] Ruleset JSON changes: regenerated `generated/*.hpp` are in sync (`git diff` clean after codegen)
- [ ] Ruleset JSON changes: `python3 codegen/validate_ruleset.py rulesets/*.json` reports OK
