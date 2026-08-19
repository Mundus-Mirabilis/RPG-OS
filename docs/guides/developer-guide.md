\page developer_guide Developer Guide

*Working on RPG OS itself — the engine, the code generator, the WASM port, or
the documentation.* This guide is for **contributors**. If you want to build a
game *on top of* RPG OS instead, the \ref user_guide "User Guide" is what
you need.

> **First, read the ground rules.** This repository has two contributor-facing
> documents that are deliberately *not* part of the published documentation:
> [`AGENTS.md`](https://github.com/Mundus-Mirabilis/RPG-OS/blob/develop/AGENTS.md)
> — the authoritative instruction file for LLM agents and contributors (ground
> rules, conventions, definition of done) — and
> [`CONTRIBUTING.md`](https://github.com/Mundus-Mirabilis/RPG-OS/blob/develop/CONTRIBUTING.md)
> (the development workflow and coding style). Everything below is the short
> version.

## Architecture in one picture

RPG OS is **header-only C++23** and consists of one shared template core that
is instantiated differently by two modes:

- **Shared template core** (`include/rpg_os/core` + `include/rpg_os/common`) —
  the engine's mechanics: `checks.hpp` (`CheckRecipe` / `resolveCheck`),
  `dice_engine.hpp` (`DiceExpression`), `variance.hpp` (ranges & variance),
  `entity.hpp` (`ResourcePool`, `StatProvider`). Used by *both* modes below.
- **Universal mode (dynamic)** — `ruleset_loader.hpp`, `engine.hpp`
  (`RulesetEngine` facade), `dynamic_entity.hpp` (`DynamicEntity`),
  `combat.hpp` (`runFight`), `game_session.hpp`.
- **Specific mode (codegen)** — `codegen/rpg_os_codegen.py` compiles a
  ruleset's schema into `generated/<ruleset>_static.hpp` (the strongly typed
  `Character` class), which loads its data at runtime via the shared
  `specific/sheet.hpp` CRTP base (`SheetBase<Character>`).

```
                ┌──────────────────────────────────────────────┐
                │  Shared template core  (core/ + common/)     │
                │  checks · dice · variance · entity · …       │
                └───────────────┬───────────────┬──────────────┘
                                │               │
              uses              │               │              uses
                                ▼               ▼
        ┌────────────────────────┐    ┌───────────────────────────┐
        │ Universal mode         │    │ Specific mode (codegen)   │
        │ RulesetEngine ·        │    │ rpg_os_codegen.py ──►     │
        │ DynamicEntity · combat │    │ generated/<ruleset>       │
        └────────────────────────┘    │ _static.hpp (Character)   │
                                      └───────────────────────────┘
```

- **Universal mode** loads any ruleset JSON at runtime and interprets it:
  an AST formula evaluator (`universal/expression.hpp`), a generic check
  resolver, a modifier pipeline, cost tables and an event system. Adding a new
  game system is writing a JSON file — no recompilation.
- **Specific mode** compiles a ruleset's *schema* into a strongly typed header
  with named members and methods. Formulas compile to C++, checks instantiate
  the shared templates with `constexpr` configs. The generated code still
  loads the JSON at **runtime** for the *data* database.
- A **parity test** asserts both modes produce identical results for the same
  scenario — a check resolved through `RulesetEngine` equals the generated
  `Character`'s named method.

## The shared template core

The core is ruleset-agnostic by design — there is no named mechanism for any
specific game anywhere in it.

- `core/concepts.hpp` defines `StatProvider`: anything with a
  `getStat(std::string_view) -> std::integral`. Universal entities use a hash
  map of `int32_t`; generated characters store attributes/skills in the
  narrowest fitting type (`uint8_t` for the shipped rulesets) and satisfy the
  concept via a string→member switch (no allocations).
- `core/checks.hpp` is a single data-driven `CheckRecipe` (resolution,
  dice, comparison, criticals, grading, difficulty) resolved by the one
  `resolveCheck` template. All shipped check styles — D&D's d20, The Dark
  Eye's 3d20 pool, BRP's percentile family — are just recipe configurations.
  A new ruleset with a new combination of dice/comparison/grading is expressed
  purely as recipe JSON.
- `core/dice_engine.hpp` provides `DiceExpression` and the injectable
  `DefaultRandom`. **Portable seeding matters**: `DefaultRandom::operator()`
  draws from the raw `std::mt19937` output (not `std::uniform_int_distribution`,
  which differs between libstdc++ and libc++). This is what makes "same seed =
  same result" hold across native and WASM (libc++) builds — the npm package's
  native-vs-WASM fight parity test depends on it. Do not "restore" the
  distribution.
- `core/math.hpp` — `floor`/`ceil`/`round`/`clamp` used by the AST evaluator
  *and* generated code, plus the non-negative fast variants
  (`floorDivN`, `ceilDivN`, `roundDivN`).
- `core/variance.hpp` — range selection (weakest … strongest / random) via
  `readVariantValue`.

## The code generator

`codegen/rpg_os_codegen.py` (Python 3, standard library only) turns a ruleset
into `generated/<ruleset_id>_static.hpp`. The outputs are **committed**; keep
them in sync with the rulesets.

- Regenerate after any schema-affecting ruleset change:
  ```sh
  python3 codegen/rpg_os_codegen.py --ruleset rulesets/dnd5e_srd.json --out generated/
  python3 codegen/rpg_os_codegen.py --ruleset rulesets/tde5e_core.json --out generated/
  python3 codegen/rpg_os_codegen.py --ruleset rulesets/brp_ugc.json --out generated/
  ```
  then run `clang-format -i generated/*.hpp`. A CI job regenerates into a temp
  dir and checks `git diff --exit-code`, so a stale generated header fails CI.
- `codegen/validate_ruleset.py` validates rulesets against
  `rulesets/ruleset.schema.json` (full draft-07 when `jsonschema` is
  installed, structural fallback otherwise). Run it after touching any
  ruleset: `python3 codegen/validate_ruleset.py rulesets/*.json`.
- The generator's tricky parts are documented in the code and in the repo
  memory (`/memories/repo/rpg-os.md`): e.g. the GCC `Type{{`-at-end-of-line
  parsing quirk, `-Wmissing-field-initializers` interactions, and the
  `_dice` literal rendering rules.

## Testing

- Tests use **doctest** (`tests/`), registered with CTest.
- **TDD is mandatory**: write a failing test first (red), implement the
  minimal code to pass (green), then refactor.
- The suite includes the universal↔specific **parity test** and codegen-output
  tests that pin the generated headers' behaviour.
- CI builds Debug + warnings-as-errors, plus an ASan/UBSan build. Keep the
  whole suite green before finishing.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## The WASM port & npm package

- `wasm/bindings.cpp` is a flat extern "C" API over the universal engine,
  compiled to WASM by `wasm/build.sh` (em++). It stays host-compilable (no
  Emscripten headers unless `__EMSCRIPTEN__`), so
  `g++ -std=c++23 -fsyntax-only wasm/bindings.cpp` works as a local syntax
  check.
- `wasm/package/` is the `@mundus-mirabilis/rpg-os` npm package, assembled by
  `wasm/build.sh` (WASM dist + rulesets + LICENSE). CI publishes it: pushes to
  `main`/`develop` under the `main`/`develop` dist-tags, `v*` tags under
  `latest`.
- The ELO ratings stay in Python (`scripts/elo_ranking.py`); the demo only
  ports the small formula to JS. See `wasm/package/README.md` and
  `web/demo/README.md`.
- **Licences**: the engine code is Apache-2.0, but ruleset JSON data is not —
  each ruleset carries its own licence (see the root `README.md` licence
  table and `docs/README.md`).

### Running the web demo locally

The **ELO Arena** demo (`web/demo/`) is a static, no-build page: the universal
engine runs in the browser through the WASM module. To debug or test it on
your machine the WASM module, the rulesets and (optionally) the ELO
leaderboards must be present under `web/demo/` — all three folders are
gitignored and normally produced by CI:

1. **Build the WASM module + copy the assets** — `wasm/build.sh` requires the
   Emscripten SDK on PATH (it prints a helpful error when `em++` is missing):
   ```sh
   wasm/build.sh
   cp wasm/package/dist/rpg-os-universal.{js,wasm} web/demo/wasm/
   cp rulesets/dnd5e_srd.json rulesets/tde5e_core.json web/demo/rulesets/
   ```
2. **Generate the ELO leaderboards** (optional — without them the page loads
   with a "Leaderboard not found" note, but the rankings and the head-to-head
   panel need them). Same fixed seed and flags as CI:
   ```sh
   cmake --build build --target rpg_os_example_fight
   python3 scripts/elo_ranking.py --ruleset rulesets/dnd5e_srd.json \
     --seed 20260817 --rounds 40 --games 20 --jobs 4 --initial 1000 \
     --json web/demo/data/dnd5e_srd_elo.json
   python3 scripts/elo_ranking.py --ruleset rulesets/tde5e_core.json \
     --seed 20260817 --rounds 40 --games 20 --jobs 4 --initial 1000 \
     --json web/demo/data/tde5e_core_elo.json
   ```
3. **Serve the folder** — `fetch()` needs http, not `file://`:
   ```sh
   python3 -m http.server -d web/demo 8000
   # open http://localhost:8000
   ```

In VS Code all of this is one click: **Terminal → Run Task… → "Run Web Demo"**
builds the assets and the leaderboards, then starts the server (a background
task) — open [http://localhost:8000](http://localhost:8000) to use the page.
For a fast page-only debug pass, run "Build Web Demo (WASM + rulesets)" and
"Serve Web Demo" instead and skip the leaderboards. To debug the page in the
browser, press **F5** ("Debug Web Demo", `.vscode/launch.json`) while the
"Serve Web Demo" task is running — it launches Chrome at the served URL with
the JS debugger attached. The step-by-step breakdown and the CI parity live
in `web/demo/README.md`.

## The documentation build

The single source of truth is the **`Doxyfile`** at the repository root; it
renders into `html/` (gitignored). The **`README.md`** is the front page
(`USE_MDFILE_AS_MAINPAGE`); this guide and the
\ref user_guide "User Guide" are the two curated entry points.

```sh
cmake --build build --target rpg_os_docs     # HTML -> ./html
```

CI deploys the docs to GitHub Pages under versioned subfolders. The full
infrastructure story — the reusable deploy workflow, the `VERSION` file that
feeds `PROJECT_NUMBER`, the branding assets, and the vendored Doxygen Awesome
theme — lives in `docs/README.md` and `docs/assets/README.md`.

Doxygen has a few sharp edges worth knowing (they are documented in the repo
memory): `\ref` inside `@file` blocks never resolves (use `@c`/backticks);
`USE_MDFILE_AS_MAINPAGE` needs an explicit `./README.md` path; raw HTML
`<img>` tags are not rewritten (use markdown image syntax); and the docs
output must be cleaned before each build (`cmake/Doxygen.cmake` does this).

## Conventions in 30 seconds

- **Licence header** on every file: `// Copyright (c) 2026 Christian Mayer
  and the Mundus Mirabilis contributors.` + `// SPDX-License-Identifier:
  Apache-2.0` (generated headers emit it automatically).
- **Naming**: the project is written "RPG OS"; identifiers use `rpg_os`
  (namespace, includes `<rpg_os/...>`, CMake `RPG_OS_*`) — never `rpgos`.
- **Style**: 2-space indent, spaces only, 100-column limit — run
  `clang-format -i` on every file you touch (never on `third_party/`).
- **Modern C++23 only**: `const`/`constexpr`/`noexcept`, value semantics,
  `std::expected` for errors, `std::string_view`/`std::span` for non-owning
  parameters, templates to share algorithms between the two modes.
- **Header-only first**: keep it that way — no out-of-line `.cpp` files; the
  `rpg_os` CMake target must stay an `INTERFACE` target.
