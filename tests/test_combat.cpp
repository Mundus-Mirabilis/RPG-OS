// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_combat.cpp
 * @brief Tests for the combat simulation helpers
 * (@c include/rpg_os/universal/combat.hpp).
 *
 * Runs against the real The Dark Eye 5e ruleset: combatant specs built from
 * the bestiary and from archetypes, armour absorption through the event
 * pipeline, and fights that run to the end (or to the round guard). Scripted
 * RNGs keep the fights deterministic, so the combat loop's RNG consumption
 * order is pinned by these tests.
 */
#include "test_util.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
#include <rpg_os/universal/combat.hpp>
#include <string>

TEST_CASE("combat: attack check type and hit-point pool are resolved") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  CHECK(rpg_os::resolveAttackCheckType(engine.ruleset()) == "tde_attack");
  CHECK(rpg_os::resolveHitPointPool(engine.ruleset()) == "LP");
}

TEST_CASE("combat: bestiary creature spec is built from its attacks and dodge") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  rpg_os::CombatantSpec spec;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "gotongi", spec));
  CHECK_FALSE(spec.isArchetype);
  CHECK(spec.name == "Gotongi");
  CHECK(spec.attackValue == 15); // best `to_hit` of its Pinch attack
  CHECK(spec.defenseValue == 9); // bestiary `dodge`
  CHECK(spec.armorRating == 0);
  CHECK(spec.damageExpression == "1d6");
}

TEST_CASE("combat: archetype spec uses derived attack/parry and a weapon") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  rpg_os::CombatantSpec spec;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "geron", spec));
  CHECK(spec.isArchetype);
  CHECK(spec.attackValue == 7);  // 6 + COU_Bonus (COU 12)
  CHECK(spec.defenseValue == 4); // floor(SR/2) + AGI_Bonus (AGI 13)
  CHECK(spec.armorRating == 2);
  CHECK(spec.damageExpression == "1d6+4"); // default longsword

  rpg_os::CombatantSpec greatsword;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "geron", greatsword, "2d6"));
  CHECK(greatsword.damageExpression == "2d6");
}

TEST_CASE("combat: a bestiary entry with attacks uses its best attack") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  rpg_os::CombatantSpec spec;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "heshthot", spec));
  CHECK_FALSE(spec.isArchetype);
  CHECK(spec.defenseValue == 7);           // bestiary `dodge`
  CHECK(spec.attackValue == 16);           // best `to_hit` (Long Sword / Whip)
  CHECK(spec.damageExpression == "1d6+5"); // the Long Sword's DP
}

TEST_CASE("combat: a bestiary entry without attacks falls back to derived values") {
  // The shipped Heshthot has attacks, so strip them from a loaded copy to
  // exercise the fallback path (no natural attack defined).
  const std::string path = rulesetPath("tde5e_core.json");
  std::ifstream file(path);
  REQUIRE(file.good());
  rpg_os::Json ruleset = rpg_os::Json::parse(
      std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
  for (rpg_os::Json &creature : ruleset["data"]["creatures"]) {
    if (creature.value("id", "") == "heshthot") {
      creature.erase("attacks");
    }
  }
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(ruleset.dump()));
  rpg_os::CombatantSpec spec;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "heshthot", spec));
  CHECK_FALSE(spec.isArchetype);
  CHECK(spec.defenseValue == 7);         // bestiary `dodge`
  CHECK(spec.attackValue == 8);          // 6 + COU_Bonus (COU 16)
  CHECK(spec.damageExpression == "1d6"); // unarmed fallback
}

TEST_CASE("combat: bestiary armour rating is applied through the damage pipeline") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  rpg_os::CombatantSpec heshthot;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "heshthot", heshthot)); // armor_rating 2
  rpg_os::DefaultRandom rng(1);
  auto fighter = rpg_os::createFighter(engine, heshthot, rng);
  REQUIRE(fighter != nullptr);
  const int32_t before = fighter->resource("LP");
  CHECK(before == 35);
  rpg_os::DynamicEntity orc(engine.ruleset(), "orc");
  CHECK(engine.applyDamage(orc, *fighter, "LP", 10) == -8); // 10 - Armor_Rating 2
  CHECK(fighter->resource("LP") == before - 8);
}

TEST_CASE("combat: the stronger beast defeats a helpless one to the end") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  rpg_os::CombatantSpec irrhalk;
  rpg_os::CombatantSpec toad;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "irrhalk", irrhalk));
  REQUIRE(rpg_os::makeCombatantSpec(engine, "toad", toad));

  // Scripted RNG: irrhalk wins initiative (3 vs 2), lands a critical hit (1)
  // that is not parried (20) and rolls maximum damage on Claws (6, 6) -> 16,
  // which is more than the toad's 2 life points.
  auto rng = script({3, 2, 1, 20, 6, 6});
  const rpg_os::FightOutcome outcome =
      rpg_os::runFight(engine, irrhalk, toad, "tde_attack", "LP", 100, rng);
  CHECK(outcome.winnerIndex == 0);
  CHECK(outcome.rounds == 1);
  CHECK(outcome.remainingLp[0] == 90); // Irrhalk untouched
  CHECK(outcome.remainingLp[1] <= 0);  // Toad at 0
}

TEST_CASE("combat: a detailed fight log records dice and stat changes") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  rpg_os::CombatantSpec irrhalk;
  rpg_os::CombatantSpec toad;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "irrhalk", irrhalk));
  REQUIRE(rpg_os::makeCombatantSpec(engine, "toad", toad));

  // Same script as the plain runFight test above: irrhalk wins initiative
  // (3 vs 2), lands a critical (1) the toad fails to parry (20), and rolls
  // maximum Claws damage (6, 6) + 4 = 16 — more than the toad's 2 LP.
  auto rng = script({3, 2, 1, 20, 6, 6});
  rpg_os::FightLog log;
  const rpg_os::FightOutcome outcome =
      rpg_os::runFight(engine, irrhalk, toad, "tde_attack", "LP", 100, rng, true, log);
  CHECK(outcome.winnerIndex == 0);
  CHECK(outcome.rounds == 1);

  CHECK(log.winnerIndex == 0);
  CHECK(log.names[0] == "Irrhalk");
  CHECK(log.names[1] == "Toad (Kosh Toad)");
  CHECK(log.maxLp[0] == 90);
  CHECK(log.maxLp[1] == 2);
  REQUIRE(log.rounds.size() == 1);

  const rpg_os::FightRoundLog &round = log.rounds[0];
  CHECK(round.round == 1);
  CHECK(round.initRoll[0] == 3); // the raw initiative dice
  CHECK(round.initRoll[1] == 2);
  CHECK(round.goesFirst == 0);        // irrhalk acts first
  REQUIRE(round.actions.size() == 1); // the toad never gets to act

  const rpg_os::FightActionLog &action = round.actions[0];
  CHECK(action.actorIndex == 0);
  CHECK(action.targetIndex == 1);
  CHECK(action.kind == "attack");
  CHECK(action.isHit);
  CHECK(action.checkDice == std::vector<int32_t>({1, 20})); // attack, then parry
  CHECK(action.damageDice == std::vector<int32_t>({6, 6}));
  CHECK(action.damage == 16);
  CHECK(action.hpBefore == 2); // the toad starts at its 2 LP
  CHECK(action.targetHp == 0); // the toad is reduced to 0 LP
}

TEST_CASE("combat: a logged fight consumes the identical RNG stream as a plain fight") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  rpg_os::CombatantSpec a;
  rpg_os::CombatantSpec b;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "geron", a));
  REQUIRE(rpg_os::makeCombatantSpec(engine, "gotongi", b));

  // The same seeded fight once without a log and once with: the log must not
  // change how many (or which) random values the fight draws, so the outcome
  // and both remaining hit-point pools are byte-identical.
  rpg_os::DefaultRandom rngPlain(42);
  const rpg_os::FightOutcome plain =
      rpg_os::runFight(engine, a, b, "tde_attack", "LP", 100, rngPlain, true);
  rpg_os::DefaultRandom rngLogged(42);
  rpg_os::FightLog log;
  const rpg_os::FightOutcome logged =
      rpg_os::runFight(engine, a, b, "tde_attack", "LP", 100, rngLogged, true, log);

  CHECK(logged.winnerIndex == plain.winnerIndex);
  CHECK(logged.rounds == plain.rounds);
  CHECK(logged.remainingLp[0] == plain.remainingLp[0]);
  CHECK(logged.remainingLp[1] == plain.remainingLp[1]);
  REQUIRE(log.rounds.size() == static_cast<std::size_t>(logged.rounds));
}

TEST_CASE("combat: a logged mage fight records the cast action and its dice") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  rpg_os::CombatantSpec magus;
  rpg_os::CombatantSpec toad;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "magister", magus));
  REQUIRE(rpg_os::makeCombatantSpec(engine, "toad", toad));

  // Same magic-path script as the existing test: the mage wins initiative
  // (5 vs 1), passes the SGC/INT/CON casting check (three 1s), and
  // Fulminictus rolls maximum damage (6, 6).
  auto rng = script({5, 1, 1, 1, 1, 6, 6});
  rpg_os::FightLog log;
  const rpg_os::FightOutcome outcome =
      rpg_os::runFight(engine, magus, toad, "tde_attack", "LP", 100, rng, true, log);
  CHECK(outcome.winnerIndex == 0);
  REQUIRE(log.rounds.size() == 1);
  REQUIRE(log.rounds[0].actions.size() == 1);

  const rpg_os::FightActionLog &action = log.rounds[0].actions[0];
  CHECK(action.actorIndex == 0);
  CHECK(action.kind == "cast");
  CHECK(action.spellId == "fulminictus");
  CHECK(action.spellName == "Fulminictus"); // the human-readable name, not the id
  CHECK(action.isHit);
  CHECK(action.checkDice == std::vector<int32_t>({1, 1, 1}));
  // The spell rolls full damage, but the recorded `damage` is the clamped
  // hit-point loss the target actually took (SpellResult::appliedDamage):
  // the toad has only 2 LP, so it loses exactly 2 despite the overkill.
  CHECK(action.damageDice == std::vector<int32_t>({6, 6})); // the 2d6 roll
  CHECK(action.damage == 2);
  CHECK(action.hpBefore == 2); // the toad starts at its 2 LP
  CHECK(action.targetHp == 0); // the toad is reduced to 0 LP
  CHECK(action.resourceId == "AE");
  CHECK(action.resourceCost == 8); // Fulminictus costs 8 AE
  // The spell resource is a pool that depletes with every cast: the mage
  // starts at its full 35 AE (20 + INT 15) and the 8-AE cast leaves 27.
  CHECK(action.resourceBefore == 35);
  CHECK(action.resourceAfter == 27);
  // The transcript header lists each combatant's known spells by name, and
  // their starting spell resource.
  REQUIRE(log.spells[0].size() == 3);
  CHECK(log.spells[0][0] == "Fulminictus");
  CHECK(log.spells[1].empty()); // the toad knows no spells
  CHECK(log.resourceId == "AE");
  CHECK(log.resourcePool[0] == 35); // the mage's full Astral Energy
  CHECK(log.resourcePool[1] == 32); // the toad also has AE (20 + INT 12), but no spells
}

TEST_CASE("combat: maxRounds guard ends a helpless fight as a draw") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  rpg_os::CombatantSpec toad;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "toad", toad)); // damage "0": can never kill
  rpg_os::DefaultRandom rng(1234);
  const rpg_os::FightOutcome outcome =
      rpg_os::runFight(engine, toad, toad, "tde_attack", "LP", 5, rng);
  CHECK(outcome.winnerIndex == -1);
  CHECK(outcome.rounds == 5);
  CHECK(outcome.remainingLp[0] > 0);
  CHECK(outcome.remainingLp[1] > 0);
}

TEST_CASE("combat: unknown combatant id is rejected") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  rpg_os::CombatantSpec spec;
  CHECK_FALSE(rpg_os::makeCombatantSpec(engine, "no_such_creature", spec));
}

TEST_CASE("combat: brp_ugc opposed percentile combat runs to the end") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("brp_ugc.json")));
  CHECK(rpg_os::resolveAttackCheckType(engine.ruleset()) == "brp_combat");
  CHECK(rpg_os::resolveHitPointPool(engine.ruleset()) == "HP");

  // Archetype: Attack = Brawl 25, Parry = Dodge 25, custom weapon.
  rpg_os::CombatantSpec humanA;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "average_human", humanA, "1d12"));
  CHECK(humanA.isArchetype);
  CHECK(humanA.attackValue == 25);
  CHECK(humanA.defenseValue == 25);

  // Scripted RNG: human A wins initiative (13 vs 12), lands a critical (1,
  // <= ceil(25/20) = 2) that is not parried (50 > 25) and rolls maximum
  // damage (12) -> the other human's 12 hit points are gone in one round.
  auto rng = script({2, 1, 1, 50, 12});
  const rpg_os::FightOutcome outcome =
      rpg_os::runFight(engine, humanA, humanA, "brp_combat", "HP", 100, rng);
  CHECK(outcome.winnerIndex == 0);
  CHECK(outcome.rounds == 1);
  CHECK(outcome.remainingLp[0] == 12); // attacker untouched
  CHECK(outcome.remainingLp[1] == 0);  // defender at 0
}

TEST_CASE("combat: a mage archetype spec carries its known spells") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  rpg_os::CombatantSpec magus;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "magister", magus));
  CHECK(magus.isArchetype);
  CHECK_FALSE(magus.spellIds.empty());
  CHECK(std::find(magus.spellIds.begin(), magus.spellIds.end(), "fulminictus") !=
        magus.spellIds.end());
  CHECK(std::find(magus.spellIds.begin(), magus.spellIds.end(), "ignifaxius") !=
        magus.spellIds.end());
}

TEST_CASE("combat: pickSpell ignores non-damaging spells even with a stale damage field") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("dnd5e_srd.json")));
  // The sphinx knows only non-damaging spells (detect, buffs, resists). Its
  // `heroes_feast` carries a stale bare `damage: "2d10"` next to its real
  // effects (a resist) — pickSpell must not treat it as a damaging spell, or
  // the sphinx would waste every turn casting it and never attack.
  rpg_os::CombatantSpec sphinx;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "sphinx_of_valor", sphinx));
  CHECK_FALSE(sphinx.spellIds.empty());
  rpg_os::DefaultRandom rng(7);
  auto fighter = rpg_os::createFighter(engine, sphinx, rng);
  REQUIRE(fighter != nullptr);
  CHECK(rpg_os::detail::pickSpell(engine, *fighter, sphinx.spellIds).empty());
}

TEST_CASE("combat: pickSpell ranks spells by affordability and expected damage") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  rpg_os::CombatantSpec magus;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "magister", magus));
  rpg_os::DefaultRandom rng(7);
  auto mage = rpg_os::createFighter(engine, magus, rng);
  REQUIRE(mage != nullptr);
  CHECK(mage->resource("AE") == 35); // 20 + INT 15
  // Full arcane energy: the strongest damaging spell wins (Fulminictus 2d6).
  CHECK(rpg_os::detail::pickSpell(engine, *mage, magus.spellIds) == "fulminictus");
  // With no arcane energy left no spell is affordable.
  (void)mage->modifyResource("AE", -mage->resource("AE"));
  CHECK(mage->resource("AE") == 0);
  CHECK(rpg_os::detail::pickSpell(engine, *mage, magus.spellIds).empty());
  // With only enough AE for the cheap spell, the best affordable one is chosen.
  (void)mage->modifyResource("AE", 4); // only Witch's Claws (4 AE) fits
  CHECK(rpg_os::detail::pickSpell(engine, *mage, magus.spellIds) == "witch_s_claws");
}

TEST_CASE("combat: with magic enabled a mage casts instead of attacking with a weapon") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  rpg_os::CombatantSpec magus;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "magister", magus));
  rpg_os::CombatantSpec toad;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "toad", toad));

  // The script pins the magic path: the mage wins initiative (5), passes the
  // SGC/INT/CON casting check (three 1s), and Fulminictus rolls maximum
  // damage (6,6) = 12 -> the toad's 2 LP are gone in one round. A weapon
  // attack consumes a different RNG sequence, so this script only completes
  // when magic (not the sword) is used.
  auto rng = script({5, 1, 1, 1, 1, 6, 6});
  const rpg_os::FightOutcome outcome =
      rpg_os::runFight(engine, magus, toad, "tde_attack", "LP", 100, rng);
  CHECK(outcome.winnerIndex == 0);
  CHECK(outcome.rounds == 1);
  CHECK(outcome.remainingLp[0] == 27); // the mage's 27 LP untouched
  CHECK(outcome.remainingLp[1] <= 0);
}

TEST_CASE("combat: magic can be disabled for a pure weapon comparison") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  rpg_os::CombatantSpec magus;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "magister", magus));
  rpg_os::CombatantSpec toad;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "toad", toad));

  // With magic disabled the same mage swings its weapon: it wins initiative
  // (6), rolls a critical attack (1) the toad fails to parry (20), and deals
  // 1d6+4 -> 10, killing the toad in one round.
  auto rng = script({6, 1, 1, 20, 6});
  const rpg_os::FightOutcome outcome =
      rpg_os::runFight(engine, magus, toad, "tde_attack", "LP", 100, rng, false);
  CHECK(outcome.winnerIndex == 0);
  CHECK(outcome.rounds == 1);
  CHECK(outcome.remainingLp[1] <= 0);
}

TEST_CASE("combat: dnd5e attack check type and hit-point pool are resolved") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("dnd5e_srd.json")));
  CHECK(rpg_os::resolveAttackCheckType(engine.ruleset()) == "dnd5e_attack");
  CHECK(rpg_os::resolveHitPointPool(engine.ruleset()) == "HP");
}

TEST_CASE("combat: dnd5e creature spec promotes ac, initiative, and its best attack") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("dnd5e_srd.json")));
  rpg_os::CombatantSpec spec;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "goblin_warrior", spec));
  CHECK_FALSE(spec.isArchetype);
  CHECK(spec.attackValue == 4);            // best `to_hit` (Scimitar / Shortbow)
  CHECK(spec.damageExpression == "1d6+2"); // its attack damage
  CHECK(spec.acValue == 15);               // bestiary `ac`
  CHECK(spec.initiativeValue == 2);        // bestiary `initiative`
}

TEST_CASE("combat: dnd5e fighter promotes real AC and initiative over the derived stats") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("dnd5e_srd.json")));
  rpg_os::CombatantSpec goblin;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "goblin_warrior", goblin));
  rpg_os::DefaultRandom rng(1);
  auto fighter = rpg_os::createFighter(engine, goblin, rng);
  REQUIRE(fighter != nullptr);
  // The real bestiary AC (15) overrides the derived 10 + DEX_mod (= 12);
  // initiative exists only via promotion (D&D has no derived Initiative stat).
  CHECK(fighter->getStat("AC") == 15);
  CHECK(fighter->getStat("Initiative") == 2);
  CHECK(fighter->getStat("Attack") == 4); // best `to_hit`
}

TEST_CASE("combat: dnd5e attack-vs-AC resolves to the end") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("dnd5e_srd.json")));
  rpg_os::CombatantSpec a;
  rpg_os::CombatantSpec b;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "goblin_warrior", a));
  REQUIRE(rpg_os::makeCombatantSpec(engine, "goblin_warrior", b));

  // Scripted RNG: both goblins roll the minimum average hit points (8), A
  // wins initiative (2 + 6 vs 2 + 3), lands its +4 attack (11 + 4 >= AC 15)
  // and rolls maximum damage (1d6+2 = 8) — killing B in one round. Two RNG
  // values are consumed up front for the creatures' average hit-point picks.
  auto rng = script({0, 0, 6, 3, 11, 6});
  const rpg_os::FightOutcome outcome =
      rpg_os::runFight(engine, a, b, "dnd5e_attack", "HP", 100, rng);
  CHECK(outcome.winnerIndex == 0);
  CHECK(outcome.rounds == 1);
  CHECK(outcome.remainingLp[0] == 8); // the winner's 8 HP untouched
  CHECK(outcome.remainingLp[1] == 0); // the loser at 0 HP
}

TEST_CASE("combat: a bestiary spellcaster's spells are carried and cast in a fight") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("dnd5e_srd.json")));
  rpg_os::CombatantSpec dragon;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "ancient_gold_dragon", dragon));
  CHECK_FALSE(dragon.isArchetype);
  // The dragon's `spells` array (its innate spellcasting) is carried by the spec.
  CHECK_FALSE(dragon.spellIds.empty());
  CHECK(std::find(dragon.spellIds.begin(), dragon.spellIds.end(), "flame_strike") !=
        dragon.spellIds.end());
  CHECK(std::find(dragon.spellIds.begin(), dragon.spellIds.end(), "guiding_bolt") !=
        dragon.spellIds.end());

  rpg_os::CombatantSpec rat;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "giant_rat", rat));
  CHECK(rat.spellIds.empty()); // the rat is pure melee

  // Scripted D&D fight: the dragon wins initiative (4 + 16 vs 5 + 3), casts
  // its strongest damaging spell (Flame Strike, 5d6), and the rat's 7 hit
  // points are gone in one round. RNG: both creatures' average hit points
  // (0..177, 0..3), both initiative rolls, the 5d6 spell damage, and the
  // rat's DEX save.
  auto rng = script({50, 2, 4, 5, 6, 1, 2, 3, 4, 11});
  rpg_os::FightLog log;
  const rpg_os::FightOutcome outcome =
      rpg_os::runFight(engine, dragon, rat, "dnd5e_attack", "HP", 20, rng, true, log);
  CHECK(outcome.winnerIndex == 0);
  CHECK(outcome.rounds == 1);
  REQUIRE(log.rounds.size() == 1);
  REQUIRE(log.rounds[0].actions.size() == 1);

  const rpg_os::FightActionLog &action = log.rounds[0].actions[0];
  CHECK(action.kind == "cast");
  CHECK(action.spellId == "flame_strike");
  CHECK(action.spellName == "Flame Strike");
  CHECK(action.isHit); // D&D spells have no casting check — the cast resolves
  CHECK(action.damageDice == std::vector<int32_t>({6, 1, 2, 3, 4}));
  CHECK(action.damage > 0);
  CHECK(action.hpBefore > 0);
  CHECK(action.targetHp == 0); // the rat is reduced to 0 HP
  // The transcript header lists the dragon's spells by name, the rat's empty.
  CHECK(log.spells[0].size() == dragon.spellIds.size());
  CHECK(std::find(log.spells[0].begin(), log.spells[0].end(), "Flame Strike") !=
        log.spells[0].end());
  CHECK(log.spells[1].empty());
}

TEST_CASE("combat: a spec built from an entity sheet fights like the same character from an id") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));

  // Build the spec the classic way (from the ruleset id) and from a live
  // entity created from the same archetype; both must describe Geron.
  rpg_os::CombatantSpec fromId;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "geron", fromId, "1d6+4"));
  // No RNG is consumed for a TDE archetype: its attribute values are constants.
  auto creationRng = script({0});
  auto geronEntity = engine.createEntityWith("geron", rpg_os::Variance::Average, creationRng);
  REQUIRE(geronEntity != nullptr);
  rpg_os::CombatantSpec fromSheet;
  REQUIRE(rpg_os::makeCombatantSpecFromEntity(engine, *geronEntity, fromSheet, "1d6+4"));

  CHECK(fromSheet.id == "geron");
  CHECK(fromSheet.name == "Geron, the Mercenary"); // resolved from the ruleset record
  CHECK(fromSheet.attackValue == fromId.attackValue);
  CHECK(fromSheet.defenseValue == fromId.defenseValue);
  CHECK(fromSheet.armorRating == fromId.armorRating);
  CHECK(fromSheet.damageExpression == fromId.damageExpression);
  CHECK(fromSheet.sheet.is_object());

  // The same scripted fight against a toad must produce identical outcomes,
  // proving a sheet-based combatant is interchangeable with the named entry.
  rpg_os::CombatantSpec toad;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "toad", toad));

  auto rngId = script({6, 1, 1, 20, 6});
  const rpg_os::FightOutcome viaId =
      rpg_os::runFight(engine, fromId, toad, "tde_attack", "LP", 100, rngId);
  auto rngSheet = script({6, 1, 1, 20, 6});
  const rpg_os::FightOutcome viaSheet =
      rpg_os::runFight(engine, fromSheet, toad, "tde_attack", "LP", 100, rngSheet);

  CHECK(viaSheet.winnerIndex == viaId.winnerIndex);
  CHECK(viaSheet.rounds == viaId.rounds);
  CHECK(viaSheet.remainingLp[0] == viaId.remainingLp[0]);
  CHECK(viaSheet.remainingLp[1] == viaId.remainingLp[1]);
}

TEST_CASE("combat: a hand-built character with no ruleset entry can fight") {
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromFile(rulesetPath("tde5e_core.json")));

  // A character that exists only as a sheet — never declared in the ruleset.
  rpg_os::DynamicEntity hero(engine.ruleset(), "my_hero");
  hero.setBaseAttribute("COU", 12);
  hero.setBaseAttribute("AGI", 13);
  hero.setBaseAttribute("CON", 13);
  hero.setBaseAttribute("Attack", 12);
  hero.setBaseAttribute("Parry", 8);
  hero.setBaseAttribute("Armor_Rating", 3);
  hero.setBaseAttribute("Initiative", 12);
  hero.refreshResources(); // give the sheet its pools (LP = 5 + 2*CON = 31)

  rpg_os::CombatantSpec heroSpec;
  REQUIRE(rpg_os::makeCombatantSpecFromEntity(engine, hero, heroSpec, "1d6+4"));
  CHECK(heroSpec.id == "my_hero");
  CHECK(heroSpec.attackValue == 12);
  CHECK(heroSpec.defenseValue == 8);
  CHECK(heroSpec.sheet.is_object());

  rpg_os::CombatantSpec toad;
  REQUIRE(rpg_os::makeCombatantSpec(engine, "toad", toad));

  // The hero wins initiative, lands a critical the toad fails to parry, and
  // rolls maximum weapon damage (1d6+4 = 10) — far more than the toad's 2 LP.
  auto rng = script({6, 1, 1, 20, 6});
  const rpg_os::FightOutcome outcome =
      rpg_os::runFight(engine, heroSpec, toad, "tde_attack", "LP", 100, rng);
  CHECK(outcome.winnerIndex == 0);
  CHECK(outcome.rounds == 1);
  CHECK(outcome.remainingLp[0] > 0); // hero untouched (the toad never acts)
  CHECK(outcome.remainingLp[1] <= 0);
}
