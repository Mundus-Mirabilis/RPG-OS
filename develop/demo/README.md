# ELO Arena demo

A static, no-build web page that demonstrates the universal RPG OS engine
compiled to WebAssembly. It is deployed to GitHub Pages under the same version
folders as the docs — `/main/demo/`, `/develop/demo/`, `/vX.Y.Z/demo/` — by the
`deploy-demo` job of `.github/workflows/docs.yml` (run *after* the docs deploy
via `needs`, so the two gh-pages pushes can never race).

## What it shows

1. **ELO ranking** — a leaderboard of every combatant in the selected ruleset
   (The Dark Eye 5e or D&D 5e SRD). The leaderboards are generated *during the
   CI run* by `scripts/elo_ranking.py --json` (the Python Monte-Carlo ELO over
   the native fight binary) and stored in `data/<ruleset>_elo.json`. Ratings
   are anchored to the ELO standard strength 1000 (`--initial 1000`); since
   ELO win probability depends only on rating *differences*, the anchor is a
   convention that keeps a default/average character at the standard strength.
   Tick exactly one row (or generate a character below) and the **Win %**
   column shows every combatant's win chance *relative to* that one — so it
   shows exactly 50% for the reference itself. The **Win %** is an ELO
   *estimate* of relative strength — combat in these systems is matchup-
   specific (a lower-rated combatant can win a particular duel), so the Head
   to head panel measures real fights for the ground-truth number.
2. **Character entry form** — attributes and skills generated from the loaded
   ruleset's schema. The entered character is ranked **live in the browser**:
   the WASM engine fights it **Swiss-style against the leaderboard combatants
   closest to its current rating** — a focus on similar strength makes the
   rating converge quickly — starting from the ELO standard strength **1000**.
   A default/average character therefore lands at the standard strength. The
   JS ELO formula (`elo.js`, a port of the Python formula) updates the rating.
   A **class selector** (rulesets that ship archetypes, e.g. The Dark Eye)
   pre-fills attributes, skills, and spells from a ruleset archetype as a
   starting point that you then edit to match your own character. A **spell
   picker** lists every spell in the ruleset (searchable, and filterable by
   class/tradition where the ruleset provides it): tick the spells your
   character knows and it really casts them in the test fights — the fight
   simulator prefers its strongest affordable damaging spell, spends the
   spell-resource pool per cast (e.g. `AE 35 → 27`), and falls back to its
   weapon only when it has no affordable damaging spell left. Only *damaging*
   spells are ever cast in a fight, so those are listed first and marked (the
   rest stay dimmed). For pool-based rulesets (TDE, BRP) an optional
   spell-resource override lets you enter your real pool (e.g. your AE)
   instead of the derived maximum; D&D has no spell-resource pool and casts
   freely.
3. **Head to head** — tick two rows; an **ELO estimate** of the win probability
   is shown (from the ratings — an estimate, because individual matchups can
   be matchup-specific), with a live 100-fight Monte-Carlo measurement running
   in the WASM engine. The live result appears in the same layout as the
   estimate (names, W/D/L record, percentages) and can be **re-run** any
   number of times — each run draws fresh dice, so you can watch the measured
   result vary around the estimate. A **"Run 1 fight — show details"** button
   replays a single fight dice by dice: every round's initiative roll, each
   combatant's attack/cast with its individual dice (attack + parry, the
   casting check, the damage dice), the hit-point changes they caused, and a
   summary of who won and in how many rounds. Spell casts are shown in full:
   the **spell's name** (e.g. "Fulminictus", not its id), the casting-check
   dice, the spell's own damage dice, the actual damage dealt, and — because
   a spell resource is a pool that runs down during a fight (e.g. The Dark
   Eye's **Astral Energy**) — the pool depleting with every cast, e.g.
   `· AE 35 → 27`, including on a failed check that still spends the round and
   the resource (the fizzle), exactly as the engine resolves it. The summary
   line shows each spellcaster's starting pool (e.g. `(AE 35)`), so you can
   see when the mage finally runs dry and falls back to its weapon. Above the
   rounds, a line lists **each combatant's known spells**, so you can see who
   is a spellcaster and what it can cast before the dice start rolling.
   Unicode icons make each line scannable at a glance: `⚔️`/`✨` open an
   attack/cast, `✅`/`❌` mark hit/success vs miss/failure, and `🏆`/`🤝`
   mark the winner or a draw. Spellcasting creatures now really cast: D&D
   bestiary spellcasters (dragons, liches, hags, ...) carry the `spells` from
   their innate spellcasting and prefer their strongest damaging spell over a
   weapon attack, so D&D fights show magic too (D&D has no spell-resource
   pool, so no `AE`-style numbers appear there). Each single fight also draws
   fresh dice, so re-running it shows a different possible fight. Long draw
   fights are truncated in the middle (first and last rounds) to stay
   readable.

The currently loaded ruleset's **own licence** is shown in a dedicated box at
the bottom of the page (read from `rpg.meta()` at runtime, so it always matches
the JSON actually loaded). The box makes the ruleset-vs-application distinction
explicit up front: it states that the rules are **not** Apache-2.0 — the
Apache-2.0 licence covers only the engine/demo code — and then shows the
licence statement, where the rights holder states it, the source document, and
the verbatim notice/attribution text the licence requires (e.g. the ORC
Notice). See the root `README.md` for the per-ruleset licence table.

## Layout

| Path | Origin |
| --- | --- |
| `index.html` `style.css` `app.js` `rpg-browser.js` `rpg-core.js` `constants.js` `transcript.js` `elo.js` | committed |
| `assets/` `favicon.png` | project branding (logo, hero banner, favicon) copied from `docs/assets/` — committed |
| `data/*_elo.json` | generated by CI (`scripts/elo_ranking.py --json`), gitignored |
| `rulesets/*.json` | copied by CI from `rulesets/`, gitignored |
| `wasm/rpg-os-universal.{js,wasm}` | built by `wasm/build.sh`, gitignored |

## Preview locally

The page needs the WASM module, the rulesets, and the leaderboards present:

```sh
# 1. Build the WASM assets (requires the Emscripten SDK on PATH).
wasm/build.sh
cp wasm/package/dist/rpg-os-universal.js wasm/package/dist/rpg-os-universal.wasm web/demo/wasm/

# 2. Copy the rulesets.
cp rulesets/dnd5e_srd.json rulesets/tde5e_core.json web/demo/rulesets/

# 3. Build the native fight binary + generate the leaderboards (fixed seed,
#    anchored to the ELO standard strength 1000; more games/pair converge
#    faster, and Swiss pairing keeps similar ratings playing each other).
cmake --build build --target rpg_os_example_fight
python3 scripts/elo_ranking.py --ruleset rulesets/dnd5e_srd.json \
  --seed 20260817 --rounds 40 --games 20 --jobs 4 --initial 1000 \
  --json web/demo/data/dnd5e_srd_elo.json
python3 scripts/elo_ranking.py --ruleset rulesets/tde5e_core.json \
  --seed 20260817 --rounds 40 --games 20 --jobs 4 --initial 1000 \
  --json web/demo/data/tde5e_core_elo.json

# 4. Serve the folder (fetch() needs http, not file://).
python3 -m http.server -d web/demo 8000
# open http://localhost:8000
```

This is exactly what the `deploy-demo` job of the CI `docs.yml` workflow
automates (after the docs deploy, so ordering is guaranteed).

In VS Code the same flow is one click: the **"Run Web Demo"** task
(`.vscode/tasks.json`) builds the WASM assets + rulesets, generates the
leaderboards and starts the server; the **"Debug Web Demo"** launch
configuration (F5, while the "Serve Web Demo" task is running) opens the page
in a browser with the JS debugger attached. See the
[Developer Guide](../../docs/guides/developer-guide.md), "Running the web
demo locally".
