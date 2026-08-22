// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_codegen_output.cpp
 * @brief Tests for the generated "specific mode" headers in @c generated/.
 *
 * These headers are produced by codegen/rpg_os_codegen.py from the rulesets.
 * They must compile, expose named members/methods, still load the JSON at
 * runtime (data database), and produce results IDENTICAL to the universal
 * engine for the same scenario — the parity guarantee that motivates the
 * whole dual-mode architecture. Any drift between the code generator and the
 * universal engine fails here.
 */
#include "test_util.hpp"

#include <brp_ugc_static.hpp>
#include <dnd5e_srd_static.hpp>
#include <doctest/doctest.h>
#include <fstream>
#include <rpg_os/universal/engine.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <tde5e_core_static.hpp>
#include <type_traits>

namespace {

using rpg_os::CheckParams;
using rpg_os::CheckResult;
using TdeCharacter = rpg_os::generated::tde5e::Character;
using DndCharacter = rpg_os::generated::dnd5e::Character;
using BrpCharacter = rpg_os::generated::brp_ugc::Character;

std::string readFile(const char *name) {
  const std::string path = std::string(RPG_OS_SOURCE_DIR) + "/rulesets/" + name;
  std::ifstream file(path);
  REQUIRE(file.good());
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

rpg_os::Json loadRuleset(const char *name) {
  return rpg_os::Json::parse(readFile(name));
}

} // namespace

TEST_CASE("generated tde5e: named members, derived getters, and resources") {
  const rpg_os::Json ruleset = loadRuleset("tde5e_core.json");
  const TdeCharacter geron = TdeCharacter::fromArchetype(ruleset, "geron");

  // Named attribute members from the archetype record.
  CHECK(geron.courage == 12);
  CHECK(geron.agility == 13);
  CHECK(geron.constitution == 13);
  CHECK(geron.armorRating == 2);

  // Named derived getters (compiled formulas).
  CHECK(geron.maxLifePoints() == 31);  // 5 + 2 * CON
  CHECK(geron.dodge() == 7);           // round(AGI / 2) = round(13 / 2)
  CHECK(geron.attackSwordsSr6() == 7); // 6 + COU_Bonus
  CHECK(geron.parrySwords() == 4);     // 3 + AGI_Bonus
  CHECK(geron.spirit() == 6);          // round((12 + 11 + 10) / 6)
  CHECK(geron.toughness() == 7);       // round((13 + 13 + 13) / 6)

  // Resources initialized to max.
  CHECK(geron.lifePoints == 31);

  // StatProvider: string -> member / method.
  CHECK(geron.getStat("COU") == 12);
  CHECK(geron.getStat("climbing") == 7);
  CHECK(geron.getStat("LifePoints_Max") == 31);
  CHECK(geron.getStat("nope") == 0);
}

TEST_CASE("generated tde5e: named skill checks match the universal engine (parity)") {
  const rpg_os::Json ruleset = loadRuleset("tde5e_core.json");
  const TdeCharacter geron = TdeCharacter::fromArchetype(ruleset, "geron");

  auto rng = script({14, 12, 11});
  const CheckResult specific = geron.checkClimbing(CheckParams{}, rng);

  CHECK(specific.isSuccess);
  CHECK(specific.remainingPool == 5);
  CHECK(specific.qualityLevel == 2);

  // The same scenario through the universal engine must give identical results.
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(readFile("tde5e_core.json")));
  auto universal = engine.createEntity("geron");
  REQUIRE(universal != nullptr);
  auto rng2 = script({14, 12, 11});
  const CheckResult universalResult =
      engine.executeSkillCheck("climbing", *universal, CheckParams{}, rng2);
  CHECK(universalResult.isSuccess == specific.isSuccess);
  CHECK(universalResult.remainingPool == specific.remainingPool);
  CHECK(universalResult.qualityLevel == specific.qualityLevel);
  CHECK(universalResult.rawDiceRolls == specific.rawDiceRolls);
}

TEST_CASE("generated dnd5e: named members, derived getters, cost table") {
  // The SRD ruleset stores text descriptions (classes/species/backgrounds/feats)
  // rather than archetype blocks, so construct a fighter directly via its
  // named members. The data database is still loaded from the JSON at runtime.
  DndCharacter fighter;
  fighter.strength = 16;
  fighter.dexterity = 14;
  fighter.constitution = 14;
  fighter.proficiencyBonus = 2;
  fighter.maxHitPoints = 12;
  fighter.hitPoints = 12;

  CHECK(fighter.strength == 16);
  CHECK(fighter.dexterity == 14);
  CHECK(fighter.strengthModifier() == 3); // floor((16-10)/2)
  CHECK(fighter.constitutionModifier() == 2);
  CHECK(fighter.armorClass() == 12); // 10 + DEX mod
  CHECK(fighter.hitPoints == 12);
  CHECK(fighter.getStat("AC") == 12);

  // Real XP -> level table.
  CHECK(DndCharacter::xpToLevel().lookup(0) == 1);
  CHECK(DndCharacter::xpToLevel().lookup(6500) == 5);
  CHECK(DndCharacter::xpToLevel().lookup(355000) == 20);
}

TEST_CASE("generated dnd5e: attack check matches the universal engine (parity)") {
  // Same fighter, built directly for the specific (generated) mode and via a
  // JSON record for the universal mode.
  DndCharacter fighter;
  fighter.strength = 16;
  fighter.dexterity = 14;
  fighter.constitution = 14;
  fighter.proficiencyBonus = 2;
  fighter.maxHitPoints = 12;
  fighter.hitPoints = 12;

  auto rng = script({10}); // 10 + STR_mod(3) + prof(2) = 15 >= AC 12
  const CheckResult specific = fighter.attackMelee(fighter, CheckParams{}, rng);
  CHECK(specific.isSuccess);
  CHECK(specific.marginOfSuccess == 3);

  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(readFile("dnd5e_srd.json")));
  rpg_os::DynamicEntity universal(engine.ruleset(), "fighter");
  rpg_os::Json record = rpg_os::Json::parse(
      R"({"attributes":{"STR":16,"DEX":14,"CON":14,"proficiency_bonus":2,"HitPoints_Max":12}})");
  universal.loadFromArchetype(record);
  auto rng2 = script({10});
  const CheckResult universalResult =
      engine.executeCheck("dnd5e_attack_melee", universal, &universal, CheckParams{}, rng2);
  CHECK(universalResult.isSuccess == specific.isSuccess);
  CHECK(universalResult.marginOfSuccess == specific.marginOfSuccess);
  CHECK(universalResult.rawDiceRolls == specific.rawDiceRolls);
}

TEST_CASE("generated tde5e: loadArchetypes reads every archetype from JSON") {
  const rpg_os::Json ruleset = loadRuleset("tde5e_core.json");
  const std::vector<TdeCharacter> heroes = TdeCharacter::loadArchetypes(ruleset);
  REQUIRE(heroes.size() == 3);
  CHECK(heroes[0].courage == 14); // louisa
  CHECK(heroes[1].courage == 12); // geron
  CHECK(heroes[2].courage == 11); // magister
  CHECK(heroes[2].intuition == 15);
}

TEST_CASE("generated code: variance-aware fromCreature (ranged hit points)") {
  const rpg_os::Json ruleset = loadRuleset("dnd5e_srd.json");

  // goblin_warrior carries "HitPoints_Max": "3d6" -> HP in [3, 18].
  auto rngWeak = script({0});
  auto rngStrong = script({0});
  const DndCharacter weakest =
      DndCharacter::fromCreature(ruleset, "goblin_warrior", rpg_os::Variance::Weakest, rngWeak);
  const DndCharacter strongest =
      DndCharacter::fromCreature(ruleset, "goblin_warrior", rpg_os::Variance::Strongest, rngStrong);
  CHECK(weakest.strength == 8); // scalar attributes unchanged
  CHECK(strongest.strength == 8);
  CHECK(weakest.maxHitPoints == 3);
  CHECK(strongest.maxHitPoints == 18);
  CHECK(weakest.hitPoints == 3);
  CHECK(strongest.hitPoints == 18);
}

TEST_CASE("generated code: loadCreatures reads the full bestiary") {
  const rpg_os::Json ruleset = loadRuleset("dnd5e_srd.json");

  // loadCreatures picks every bestiary entry (full SRD bestiary).
  const std::vector<DndCharacter> monsters = DndCharacter::loadCreatures(ruleset);
  REQUIRE(monsters.size() == 330);
  // The SRD bestiary is alphabetical: the first creature is the aboleth
  // (20d10 + 40, so HP falls in [60, 240]).
  CHECK(monsters[0].maxHitPoints >= 60);
  CHECK(monsters[0].maxHitPoints <= 240);
}

TEST_CASE("generated brp_ugc: named members, derived getters, and resources") {
  const rpg_os::Json ruleset = loadRuleset("brp_ugc.json");
  const BrpCharacter human = BrpCharacter::fromArchetype(ruleset, "average_human");

  // Named attribute members from the archetype record.
  CHECK(human.strength == 11);
  CHECK(human.constitution == 11);
  CHECK(human.size == 13);
  CHECK(human.power == 11);
  CHECK(human.dexterity == 11);
  CHECK(human.luck == 50);
  CHECK(human.sanity == 50);

  // Named skill members.
  CHECK(human.brawl == 25);
  CHECK(human.dodge == 25);
  CHECK(human.spot == 25);
  CHECK(human.language == 50);

  // Named derived getters (compiled formulas).
  CHECK(human.maxHitPoints() == 12);   // ceil((CON 11 + SIZ 13) / 2)
  CHECK(human.maxPowerPoints() == 11); // = POW
  CHECK(human.strengthX5() == 55);     // STR * 5
  CHECK(human.dexterityX5() == 55);    // DEX * 5
  CHECK(human.attack() == 25);         // = Brawl
  CHECK(human.parryDefense() == 25);   // = Dodge
  CHECK(human.initiative() == 11);     // = DEX

  // Resources initialized to max.
  CHECK(human.hitPoints == 12);
  CHECK(human.powerPoints == 11);

  // StatProvider: string -> member / method.
  CHECK(human.getStat("STR") == 11);
  CHECK(human.getStat("brawl") == 25);
  CHECK(human.getStat("HitPoints_Max") == 12);
  CHECK(human.getStat("nope") == 0);
}

TEST_CASE("generated brp_ugc: skill checks match the universal engine (parity)") {
  const rpg_os::Json ruleset = loadRuleset("brp_ugc.json");
  const BrpCharacter human = BrpCharacter::fromArchetype(ruleset, "average_human");

  auto rng = script({5}); // Brawl 25 / 5 = 5 -> Special
  const CheckResult specific = human.skillBrawl(CheckParams{}, rng);
  CHECK(specific.isSuccess);
  CHECK(specific.successLevel == rpg_os::SuccessLevel::Special);
  CHECK(specific.marginOfSuccess == 20);

  // The same scenario through the universal engine must give identical results.
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(readFile("brp_ugc.json")));
  auto universal = engine.createEntity("average_human");
  REQUIRE(universal != nullptr);
  auto rng2 = script({5});
  const CheckResult universalResult =
      engine.executeCheck("brp_skill_brawl", *universal, nullptr, CheckParams{}, rng2);
  CHECK(universalResult.isSuccess == specific.isSuccess);
  CHECK(universalResult.successLevel == specific.successLevel);
  CHECK(universalResult.marginOfSuccess == specific.marginOfSuccess);
  CHECK(universalResult.rawDiceRolls == specific.rawDiceRolls);
}

TEST_CASE("generated brp_ugc: resistance rolls match the universal engine (parity)") {
  const rpg_os::Json ruleset = loadRuleset("brp_ugc.json");
  const BrpCharacter human = BrpCharacter::fromArchetype(ruleset, "average_human");

  // Equal POW (11 vs 11): chance 50; roll 40 succeeds.
  auto rng = script({40});
  const CheckResult specific = human.resistancePow(human, CheckParams{}, rng);
  CHECK(specific.isSuccess);

  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(readFile("brp_ugc.json")));
  auto universal = engine.createEntity("average_human");
  REQUIRE(universal != nullptr);
  auto rng2 = script({40});
  const CheckResult universalResult =
      engine.executeCheck("brp_resistance_pow", *universal, universal.get(), CheckParams{}, rng2);
  CHECK(universalResult.isSuccess == specific.isSuccess);
  CHECK(universalResult.rawDiceRolls == specific.rawDiceRolls);
}

TEST_CASE("generated brp_ugc: opposed combat matches the universal engine (parity)") {
  const rpg_os::Json ruleset = loadRuleset("brp_ugc.json");
  const BrpCharacter human = BrpCharacter::fromArchetype(ruleset, "average_human");

  // Attack 2 is Critical (2 <= ceil(25/20)); parry 10 is Success -> hit.
  auto rng = script({2, 10});
  const CheckResult specific = human.combat(human, CheckParams{}, rng);
  CHECK(specific.isSuccess);
  CHECK(specific.successLevel == rpg_os::SuccessLevel::Critical);

  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(readFile("brp_ugc.json")));
  auto universal = engine.createEntity("average_human");
  REQUIRE(universal != nullptr);
  auto rng2 = script({2, 10});
  const CheckResult universalResult =
      engine.executeCheck("brp_combat", *universal, universal.get(), CheckParams{}, rng2);
  CHECK(universalResult.isSuccess == specific.isSuccess);
  CHECK(universalResult.successLevel == specific.successLevel);
  CHECK(universalResult.rawDiceRolls == specific.rawDiceRolls);
}

TEST_CASE("generated brp_ugc: loadArchetypes reads the average human") {
  const rpg_os::Json ruleset = loadRuleset("brp_ugc.json");
  const std::vector<BrpCharacter> humans = BrpCharacter::loadArchetypes(ruleset);
  REQUIRE(humans.size() == 1);
  CHECK(humans[0].strength == 11);
  CHECK(humans[0].brawl == 25);
}

TEST_CASE("generated characters store narrow stat types (no wasted int32)") {
  // Attributes and skills fit in a single byte for the shipped rulesets, so
  // the generated characters store them as uint8_t — the templates in
  // core/checks.hpp and the StatProvider concept work with any integral
  // storage type. Wide base stats (armor, hit-point maxima, level) and
  // resource pools stay int32_t where they can genuinely grow.
  static_assert(std::is_same_v<decltype(TdeCharacter::courage), uint8_t>,
                "TDE attributes must be byte-sized");
  static_assert(std::is_same_v<decltype(TdeCharacter::climbing), uint8_t>,
                "TDE skills must be byte-sized");
  static_assert(std::is_same_v<decltype(TdeCharacter::armorRating), int32_t>,
                "armor rating stays wide");
  static_assert(std::is_same_v<decltype(TdeCharacter::lifePoints), int32_t>,
                "resource pools stay wide");
  static_assert(std::is_same_v<decltype(DndCharacter::strength), uint8_t>,
                "D&D attributes must be byte-sized");
  static_assert(std::is_same_v<decltype(DndCharacter::hitPoints), int32_t>,
                "D&D hit points stay wide");
  static_assert(std::is_same_v<decltype(BrpCharacter::brawl), uint8_t>,
                "BRP skills must be byte-sized");
  static_assert(std::is_same_v<decltype(BrpCharacter::sanity), uint8_t>,
                "BRP Sanity (0..100) fits a byte");
  static_assert(std::is_same_v<decltype(TdeCharacter::money), rpg_os::Money>,
                "wealth uses the shared Money type");
  static_assert(std::is_same_v<decltype(TdeCharacter::advancement), rpg_os::Advancement>,
                "advancement uses the shared Advancement type");
  static_assert(std::is_same_v<decltype(TdeCharacter::inventory), rpg_os::Inventory>,
                "inventory uses the shared Inventory type");
  static_assert(std::is_same_v<decltype(TdeCharacter::equipment), rpg_os::Equipment>,
                "gear uses the shared Equipment type");
  static_assert(std::is_same_v<decltype(TdeCharacter::spellbook), rpg_os::Spellbook>,
                "spells use the shared Spellbook type");

  // The byte-sized storage keeps the hot stat block small; the sheet
  // bookkeeping members (inventory/equipment/spellbook/money/advancement) and
  // the living-sheet runtime state (conditions map, effect timeline,
  // resistances, traits, afflictions) are all fixed-size handles over heap
  // state, so the whole object stays compact.
  CHECK(sizeof(TdeCharacter) <= 1024);
}

TEST_CASE("generated tde5e: wealth and starting gear load from the archetype (parity)") {
  const rpg_os::Json ruleset = loadRuleset("tde5e_core.json");

  // Specific mode: fromArchetype loads the bookkeeping fields too.
  const TdeCharacter geron = TdeCharacter::fromArchetype(ruleset, "geron");
  CHECK(geron.money.baseUnits() == 2500); // 25 Silbertaler
  CHECK(geron.coinAmount("silbertaler") == 25);
  CHECK(geron.equipment.itemIn("weapon_hand") == "longsword");
  CHECK(geron.equipment.itemIn("body_armor") == "leather_armor");

  // The typed money helpers agree with the shared Money type.
  TdeCharacter rich;
  rich.depositCoins({{"dukat", 1}, {"kreutzer", 3}});
  CHECK(rich.money.baseUnits() == 200 + 30);
  CHECK(rich.coinAmount("heller") == 230);

  // Universal mode: the same record yields the same wealth (parity).
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(readFile("tde5e_core.json")));
  const auto universal = engine.createEntity("geron");
  REQUIRE(universal != nullptr);
  CHECK(universal->money().baseUnits() == 2500);
  CHECK(universal->equipment().itemIn("weapon_hand") == "longsword");
}

TEST_CASE("generated code: data section loaders expose spells/poisons/etc.") {
  const rpg_os::Json tde = loadRuleset("tde5e_core.json");
  const std::vector<rpg_os::Json> spells = TdeCharacter::loadSpells(tde);
  REQUIRE_FALSE(spells.empty());
  const rpg_os::Json &balsam = spells[0];
  CHECK(balsam.value("id", "") == "analyze_arcane_structure");
  CHECK(TdeCharacter::loadPoisons(tde).size() >= 5);
  CHECK(TdeCharacter::loadDiseases(tde).size() >= 4);
  CHECK(TdeCharacter::loadConditions(tde).size() >= 4);
  CHECK(TdeCharacter::loadItems(tde).size() >= 4);
  // A missing section yields an empty vector rather than a crash.
  CHECK(TdeCharacter::loadSection(tde, "no_such_section").empty());

  const rpg_os::Json brp = loadRuleset("brp_ugc.json");
  const std::vector<rpg_os::Json> brpSpells = BrpCharacter::loadSpells(brp);
  REQUIRE_FALSE(brpSpells.empty());
  CHECK(BrpCharacter::loadItems(brp).size() >= 20);

  const rpg_os::Json dnd = loadRuleset("dnd5e_srd.json");
  CHECK(DndCharacter::loadSpells(dnd).size() >= 300);
  CHECK(DndCharacter::loadPoisons(dnd).size() >= 10);
  CHECK(DndCharacter::loadConditions(dnd).size() >= 10);
}

TEST_CASE("generated tde5e: toJson/restoreFromJson round-trips living state") {
  const rpg_os::Json ruleset = loadRuleset("tde5e_core.json");
  TdeCharacter geron = TdeCharacter::fromArchetype(ruleset, "geron");
  REQUIRE(geron.courage == 12);

  // Put the character into a non-default living state: spent pools, temp HP,
  // a timed condition, resistances, inventory, wealth, and a known spell.
  geron.lifePoints = 20;
  geron.arcaneEnergy = 11;
  geron.tempHitPoints = 5;
  geron.conditions["pain"] = 2;
  geron.resistances.insert("Fire");
  geron.effects.add(rpg_os::ActiveEffect{"pain", 1, 2, "test"});
  geron.inventory.add(rpg_os::ItemInstance{"dagger", 2, {}});
  geron.money = rpg_os::Money{1000};
  geron.spellbook.learn("fulminictus");
  geron.terrain = "water";

  rpg_os::Json save;
  geron.toJson(save);
  CHECK(save["resources"]["LP"] == 20);
  CHECK(save["conditions"]["pain"] == 2);
  CHECK(save["temp_hp"] == 5);
  CHECK(save["terrain"] == "water");

  // A fresh character restores the exact living state.
  TdeCharacter restored;
  restored.restoreFromJson(save);
  CHECK(restored.courage == 12);
  CHECK(restored.lifePoints == 20);
  CHECK(restored.arcaneEnergy == 11);
  CHECK(restored.tempHitPoints == 5);
  CHECK(restored.conditions["pain"] == 2);
  CHECK(restored.resistances.count("Fire") == 1);
  CHECK(restored.inventory.count("dagger") == 2);
  CHECK(restored.money.baseUnits() == 1000);
  CHECK(restored.spellbook.knows("fulminictus"));
  CHECK(restored.terrain == "water");
  bool painDuration = false;
  for (const rpg_os::ActiveEffect &effect : restored.effects.effects()) {
    if (effect.conditionId == "pain" && effect.remaining == 2) {
      painDuration = true;
    }
  }
  CHECK(painDuration);
}

TEST_CASE("generated tde5e: generated save restores into the universal engine (parity)") {
  const rpg_os::Json ruleset = loadRuleset("tde5e_core.json");
  TdeCharacter geron = TdeCharacter::fromArchetype(ruleset, "geron");
  geron.lifePoints = 20;
  geron.tempHitPoints = 5;
  geron.conditions["pain"] = 2;
  geron.resistances.insert("Fire");
  geron.terrain = "land";
  rpg_os::Json save;
  geron.toJson(save);

  // The same save state restores into a universal DynamicEntity: the save
  // format is portable between the two modes.
  rpg_os::RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(readFile("tde5e_core.json")));
  auto universal = engine.createEntity("geron");
  REQUIRE(universal != nullptr);
  universal->setEventsSuppressed(true);
  universal->fromJson(save);
  universal->setEventsSuppressed(false);
  CHECK(universal->resource("LP") == 20);
  CHECK(universal->temporaryHitPoints() == 5);
  CHECK(universal->conditionStacks("pain") == 2);
  CHECK(universal->hasResistance("Fire"));
  CHECK(universal->getStat("COU") == 12);
  CHECK(universal->terrain() == "land");
}
