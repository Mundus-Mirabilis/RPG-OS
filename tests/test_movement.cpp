// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_movement.cpp
 * @brief Terrain-aware movement and the surrounding's effect on a sheet.
 */
#include "test_fixtures.hpp"

#include <algorithm>

using rpg_os::MovementOption;
using rpg_os::MovementStatus;
using rpg_os::TerrainItemEffect;

namespace {
/// A mini ruleset with three movement modes (walk/swim/fly), a land and a
/// water terrain (water: no regeneration, walking there needs breath_water),
/// an exhaustion pool, and a load-based speed reduction. Carries one item
/// that grants breath_water when worn in water.
inline std::string movementRuleset() {
  return R"({
    "schema_version": 1, "ruleset_id": "mini", "licence": "test",
    "attributes": [{"id": "STR", "name": "Strength", "min": 1, "max": 30, "default": 10},
                   {"id": "DEX", "name": "Dexterity", "min": 1, "max": 30, "default": 10}],
    "resource_pools": [{"id": "STAM", "name": "Stamina", "max_stat": "STR", "min": 0},
                       {"id": "HP", "name": "Hit Points", "max_stat": "STR", "min": 0}],
    "encumbrance": {"weight_unit": "lb", "default_weight": 1.0, "capacity": "15 * STR", "levels": []},
    "equipment_slots": [{"id": "neck", "name": "Neck"}],
    "movement": {
      "default_terrain": "land",
      "modes": {
        "walk": {"name": "Walk", "speed": "30", "exhaustion": 0},
        "swim": {"name": "Swim", "speed": "20", "exhaustion": 2},
        "fly":  {"name": "Fly", "speed": "60", "exhaustion": 1}
      },
      "terrains": {
        "land": {
          "name": "Land",
          "modes": {
            "walk": {"factor": 1.0},
            "swim": {"factor": 1.0, "possible": false},
            "fly":  {"factor": 1.0}
          }
        },
        "water": {
          "name": "Water",
          "regeneration": "none",
          "modes": {
            "walk": {"factor": 0.5, "requires": "breath_water"},
            "swim": {"factor": 1.0},
            "fly":  {"factor": 1.0}
          }
        }
      },
      "exhaustion": {"pool": "STAM", "per_distance": 1},
      "load": {"levels": [{"max_ratio": 0.5, "factor": 1.0}, {"max_ratio": 1.0, "factor": 0.5}]}
    },
    "data": {
      "items": [
        {"id": "amulet", "name": "Water Breathing Amulet", "slot": "neck", "weight": "1 lb.",
         "terrain": {"water": {"grants": ["breath_water"]}}}
      ]
    }
  })";
}
} // namespace

TEST_CASE("movement: a ruleset without movement rules is unconstrained") {
  const std::string rulesetJson = R"({
    "schema_version": 1, "ruleset_id": "mini", "licence": "test",
    "attributes": [{"id": "STR", "name": "Strength", "min": 1, "max": 30, "default": 10}],
    "resource_pools": [{"id": "HP", "name": "Hit Points", "max_stat": "STR", "min": 0}]
  })";
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(rulesetJson));
  const auto &ruleset = engine.ruleset();
  DynamicEntity sheet(ruleset, "test");
  sheet.setBaseAttribute("STR", 10);
  sheet.refreshResources();

  CHECK_FALSE(engine.hasMovement());
  const auto status = engine.movementStatus(sheet);
  REQUIRE(status.has_value());
  REQUIRE(status->options.size() == 1);
  CHECK(status->options[0].modeId == "walk");
  CHECK(status->options[0].possible);
  CHECK(status->options[0].speed == doctest::Approx(0.0)); // unconstrained
  CHECK_FALSE(status->loadReducesSpeed);

  // Any mode is possible, unconstrained, and free.
  const auto swim = engine.movementSpeed(sheet, "swim");
  REQUIRE(swim.has_value());
  CHECK(swim->possible);
  CHECK(swim->speed == doctest::Approx(0.0));
  CHECK(swim->exhaustionPerUnit == doctest::Approx(0.0));
  CHECK(*engine.movementCost(sheet, "swim", 100.0) == doctest::Approx(0.0));
  CHECK(engine.canRegenerate(sheet));

  // Moving does nothing but report an unconstrained outcome.
  const auto outcome = engine.move(sheet, "walk", 10.0);
  REQUIRE(outcome.has_value());
  CHECK(outcome->possible);
  CHECK(outcome->exhaustion == doctest::Approx(0.0));

  // A long rest still restores fully.
  (void)sheet.modifyResource("HP", -5);
  engine.longRest(sheet);
  CHECK(sheet.resource("HP") == 10);
}

TEST_CASE("movement: modes report speed and possibility per terrain") {
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(movementRuleset()));
  const auto &ruleset = engine.ruleset();
  DynamicEntity sheet(ruleset, "test");
  sheet.setBaseAttribute("STR", 10);
  sheet.refreshResources();

  CHECK(engine.hasMovement());
  CHECK(engine.effectiveTerrain(sheet) == "land");

  // walk on land: full 30.
  const auto walk = engine.movementSpeed(sheet, "walk", "land");
  REQUIRE(walk.has_value());
  CHECK(walk->possible);
  CHECK(walk->baseSpeed == doctest::Approx(30.0));
  CHECK(walk->speed == doctest::Approx(30.0));
  CHECK(walk->exhaustionPerUnit == doctest::Approx(0.0));
  CHECK(walk->regeneration);

  // swim on land is forbidden by the terrain.
  const auto swim = engine.movementSpeed(sheet, "swim", "land");
  REQUIRE(swim.has_value());
  CHECK_FALSE(swim->possible);
  CHECK(swim->reason == "not possible in land");

  // fly on land: 60.
  const auto fly = engine.movementSpeed(sheet, "fly", "land");
  REQUIRE(fly.has_value());
  CHECK(fly->speed == doctest::Approx(60.0));

  // walk in water needs breath_water, which the sheet lacks.
  const auto wade = engine.movementSpeed(sheet, "walk", "water");
  REQUIRE(wade.has_value());
  CHECK_FALSE(wade->possible);
  CHECK(wade->reason == "requires breath_water");

  // swim in water: full 20.
  const auto swimWater = engine.movementSpeed(sheet, "swim", "water");
  REQUIRE(swimWater.has_value());
  CHECK(swimWater->possible);
  CHECK(swimWater->speed == doctest::Approx(20.0));

  // unknown mode / terrain are typed errors
  CHECK(engine.movementSpeed(sheet, "burrow", "land").error() == BookkeepingError::UnknownMode);
  CHECK(engine.movementSpeed(sheet, "walk", "bog").error() == BookkeepingError::UnknownTerrain);

  // movementStatus lists every mode for the terrain
  const auto status = engine.movementStatus(sheet, "land");
  REQUIRE(status.has_value());
  CHECK(status->terrainId == "land");
  CHECK(status->options.size() == 3);
}

TEST_CASE("movement: an equipped item grants the terrain-required capability") {
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(movementRuleset()));
  const auto &ruleset = engine.ruleset();
  DynamicEntity sheet(ruleset, "test");
  sheet.setBaseAttribute("STR", 10);
  sheet.refreshResources();

  REQUIRE(engine.addItem(sheet, "amulet", 1).has_value());

  // On land the amulet does nothing special.
  CHECK_FALSE(sheet.hasCapability("breath_water"));
  const auto onLand = engine.movementSpeed(sheet, "walk", "water");
  REQUIRE(onLand.has_value());
  CHECK_FALSE(onLand->possible);

  // Carrying (but not wearing) the amulet does not grant breath_water.
  sheet.setTerrain("water");
  CHECK_FALSE(sheet.hasCapability("breath_water"));
  CHECK(sheet.itemTerrainStatus("amulet").grants.size() == 1);

  // Once equipped, the amulet grants breath_water in water automatically.
  REQUIRE(engine.equip(sheet, "neck", "amulet").has_value());
  CHECK(sheet.hasCapability("breath_water"));

  // ...which makes walking in water possible (at the terrain's half speed).
  const auto wade = engine.movementSpeed(sheet, "walk", "water");
  REQUIRE(wade.has_value());
  CHECK(wade->possible);
  CHECK(wade->speed == doctest::Approx(15.0)); // 30 x 0.5

  // Leaving the water removes the grant.
  sheet.setTerrain("land");
  CHECK_FALSE(sheet.hasCapability("breath_water"));
}

TEST_CASE("movement: terrain regeneration and long-rest interaction") {
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(movementRuleset()));
  const auto &ruleset = engine.ruleset();
  DynamicEntity sheet(ruleset, "test");
  sheet.setBaseAttribute("STR", 10);
  sheet.refreshResources();

  CHECK(engine.canRegenerate(sheet, "land"));
  CHECK_FALSE(engine.canRegenerate(sheet, "water"));
  CHECK(engine.terrainRegenerationScale(sheet, "land") == doctest::Approx(1.0));
  CHECK(engine.terrainRegenerationScale(sheet, "water") == doctest::Approx(0.0));

  // Damage in water, then rest in water: nothing regenerates.
  sheet.setTerrain("water");
  (void)sheet.modifyResource("HP", -6);
  CHECK(sheet.resource("HP") == 4);
  engine.longRest(sheet);
  CHECK(sheet.resource("HP") == 4);

  // Back on land a long rest restores fully.
  sheet.setTerrain("land");
  engine.longRest(sheet);
  CHECK(sheet.resource("HP") == 10);
}

TEST_CASE("movement: move drains the exhaustion pool and the load slows speed") {
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(movementRuleset()));
  const auto &ruleset = engine.ruleset();
  DynamicEntity sheet(ruleset, "test");
  sheet.setBaseAttribute("STR", 10);
  sheet.refreshResources();

  CHECK(sheet.resource("STAM") == 10);

  // Swim 3 units in water: cost 2/unit x 3 = 6 stamina.
  const auto outcome = engine.move(sheet, "swim", 3.0, "water");
  REQUIRE(outcome.has_value());
  CHECK(outcome->possible);
  CHECK(outcome->exhaustion == doctest::Approx(6.0));
  CHECK(outcome->timeUnits == doctest::Approx(3.0 / 20.0));
  CHECK(sheet.resource("STAM") == 4);

  // An impossible move is refused before any resource is spent.
  const auto wade = engine.move(sheet, "walk", 1.0, "water");
  REQUIRE(wade.has_value());
  CHECK_FALSE(wade->possible);
  CHECK(sheet.resource("STAM") == 4);

  // Over half capacity (10 lb carried / 150 capacity would be ratio 0.066,
  // so load the sheet to > 150 lb): walk speed halves.
  REQUIRE(engine.addItem(sheet, "amulet", 200).has_value()); // 200 lb
  const auto loaded = engine.movementSpeed(sheet, "walk", "land");
  REQUIRE(loaded.has_value());
  CHECK(loaded->speed == doctest::Approx(15.0)); // 30 x 0.5 load factor
  const auto status = engine.movementStatus(sheet, "land");
  REQUIRE(status.has_value());
  CHECK(status->loadReducesSpeed);
}

TEST_CASE("movement: terrain state round-trips through the save form") {
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(movementRuleset()));
  const auto &ruleset = engine.ruleset();
  DynamicEntity sheet(ruleset, "test");
  sheet.setBaseAttribute("STR", 10);
  sheet.refreshResources();
  sheet.setTerrain("water");

  rpg_os::Json saved;
  sheet.toJson(saved);
  DynamicEntity restored(ruleset, "test");
  restored.fromJson(saved);
  CHECK(restored.terrain() == "water");
}

TEST_CASE("movement: the shipped D&D ruleset drives movement from its data") {
  RulesetEngine dnd;
  REQUIRE(dnd.loadRulesetFromFile(rulesetPath("dnd5e_srd.json")));
  const auto &ruleset = dnd.ruleset();
  DynamicEntity sheet(ruleset, "fighter");
  sheet.setBaseAttribute("STR", 10);

  CHECK(dnd.hasMovement());
  const auto walk = dnd.movementSpeed(sheet, "walk", "land");
  REQUIRE(walk.has_value());
  CHECK(walk->possible);
  CHECK(walk->speed == doctest::Approx(30.0));

  // Walking into deep water requires breath_water (the SRD's amphibious rule).
  const auto wade = dnd.movementSpeed(sheet, "walk", "water");
  REQUIRE(wade.has_value());
  CHECK_FALSE(wade->possible);

  // Swim is possible in water at full speed.
  const auto swim = dnd.movementSpeed(sheet, "swim", "water");
  REQUIRE(swim.has_value());
  CHECK(swim->possible);
  CHECK(swim->speed == doctest::Approx(30.0));

  // A heavily loaded fighter (200 lb > 150 lb capacity) moves at ~a third of
  // the speed (the movement load levels: ratio > 1.0 -> factor 0.33).
  REQUIRE(dnd.addItem(sheet, "club", 100).has_value());
  const auto loaded = dnd.movementSpeed(sheet, "walk", "land");
  REQUIRE(loaded.has_value());
  CHECK(loaded->speed == doctest::Approx(30.0 * 0.33));

  // Land regeneration is normal.
  CHECK(dnd.canRegenerate(sheet, "land"));
}

namespace {
/// A mini ruleset carrying a creature record with structured movement/senses.
inline std::string creatureRuleset() {
  return R"({
    "schema_version": 1, "ruleset_id": "mini", "licence": "test",
    "attributes": [{"id": "STR", "name": "Strength", "min": 1, "max": 30, "default": 10}],
    "resource_pools": [{"id": "HP", "name": "Hit Points", "max_stat": "STR", "min": 0}],
    "movement": {
      "default_terrain": "land",
      "modes": {
        "walk": {"name": "Walk", "speed": "30", "exhaustion": 0},
        "swim": {"name": "Swim", "speed": "20", "exhaustion": 2},
        "fly":  {"name": "Fly", "speed": "60", "exhaustion": 1}
      },
      "terrains": {
        "land": {"name": "Land",
                 "modes": {"walk": {"factor": 1.0}, "swim": {"factor": 1.0}, "fly": {"factor": 1.0}}}
      }
    },
    "data": {
      "creatures": [
        {"id": "dragon", "name": "Dragon",
         "speed": {"walk": 40, "fly": {"value": 90, "hover": true}, "swim": 40},
         "senses": {"blindsight": 30, "darkvision": 120,
                     "truesight": {"range": 60, "note": "sees all"}},
         "passive_perception": 18}
      ]
    }
  })";
}
} // namespace

TEST_CASE("movement: a creature's own speed overrides the ruleset mode") {
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(creatureRuleset()));
  auto creature = engine.createCreature("dragon", rpg_os::Variance::Average);
  REQUIRE(creature != nullptr);

  // walk on land uses the creature's 40 (the ruleset mode is 30).
  const auto walk = engine.movementSpeed(*creature, "walk", "land");
  REQUIRE(walk.has_value());
  CHECK(walk->baseSpeed == doctest::Approx(40.0));
  CHECK(walk->speed == doctest::Approx(40.0));

  // A mode the creature does not declare falls back to the ruleset mode.
  // (climb is not in this mini ruleset, so it is an UnknownMode even though
  // the creature has its own modes.)
  CHECK(engine.movementSpeed(*creature, "climb", "land").error() == BookkeepingError::UnknownMode);

  // The creature's own movement modes are listed in movementStatus.
  const auto status = engine.movementStatus(*creature, "land");
  REQUIRE(status.has_value());
  CHECK(status->options.size() == 3);
}

TEST_CASE("movement: hover in a creature's fly speed is preserved") {
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(creatureRuleset()));
  auto creature = engine.createCreature("dragon", rpg_os::Variance::Average);
  REQUIRE(creature != nullptr);

  const auto &speeds = creature->movementSpeeds();
  const auto it = speeds.find("fly");
  REQUIRE(it != speeds.end());
  CHECK(it->second.value == doctest::Approx(90.0));
  CHECK(it->second.hover);
  // walk is a plain number (no hover).
  CHECK_FALSE(speeds.at("walk").hover);
}

TEST_CASE("movement: senses are queryable and fold into capabilities") {
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(creatureRuleset()));
  auto creature = engine.createCreature("dragon", rpg_os::Variance::Average);
  REQUIRE(creature != nullptr);

  CHECK(creature->hasSense("darkvision"));
  const auto *darkvision = creature->findSense("darkvision");
  REQUIRE(darkvision != nullptr);
  CHECK(darkvision->range == doctest::Approx(120.0));
  CHECK_FALSE(creature->hasSense("tremorsense"));
  CHECK(creature->passivePerception() == 18);

  // A sense with a note keeps its note and range.
  const auto *truesight = creature->findSense("truesight");
  REQUIRE(truesight != nullptr);
  CHECK(truesight->range == doctest::Approx(60.0));
  CHECK(truesight->note == "sees all");

  // Senses are also capabilities (so a terrain can require "darkvision").
  CHECK(creature->hasCapability("darkvision"));
}

TEST_CASE("movement: creature movement/senses round-trip through the save form") {
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(creatureRuleset()));
  auto creature = engine.createCreature("dragon", rpg_os::Variance::Average);
  REQUIRE(creature != nullptr);

  rpg_os::Json saved;
  creature->toJson(saved);
  DynamicEntity restored(engine.ruleset(), "restored");
  restored.fromJson(saved);
  CHECK(restored.movementSpeed("walk") == doctest::Approx(40.0));
  CHECK(restored.movementSpeed("fly") == doctest::Approx(90.0));
  CHECK(restored.hasSense("blindsight"));
  CHECK(restored.passivePerception() == 18);
}

TEST_CASE("movement: legacy prose speed/senses are parsed best-effort") {
  const std::string rulesetJson = R"({
    "schema_version": 1, "ruleset_id": "mini", "licence": "test",
    "attributes": [{"id": "STR", "name": "Strength", "min": 1, "max": 30, "default": 10}],
    "resource_pools": [{"id": "HP", "name": "Hit Points", "max_stat": "STR", "min": 0}],
    "movement": {
      "default_terrain": "land",
      "modes": {
        "walk": {"name": "Walk", "speed": "30", "exhaustion": 0},
        "swim": {"name": "Swim", "speed": "20", "exhaustion": 2},
        "fly":  {"name": "Fly", "speed": "60", "exhaustion": 1}
      },
      "terrains": {
        "land": {"name": "Land",
                 "modes": {"walk": {"factor": 1.0}, "swim": {"factor": 1.0}, "fly": {"factor": 1.0}}}
      }
    },
    "data": {
      "creatures": [
        {"id": "young_dragon", "name": "Young Dragon",
         "speed": "40 ft., Fly 80 ft., Swim 40 ft.",
         "senses": "Blindsight 30 ft., Darkvision 120 ft.; Passive Perception 18"}
      ]
    }
  })";
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(rulesetJson));
  auto creature = engine.createCreature("young_dragon", rpg_os::Variance::Average);
  REQUIRE(creature != nullptr);

  CHECK(creature->movementSpeed("walk") == doctest::Approx(40.0));
  CHECK(creature->movementSpeed("fly") == doctest::Approx(80.0));
  CHECK(creature->movementSpeed("swim") == doctest::Approx(40.0));
  CHECK(creature->hasSense("darkvision"));
  CHECK(creature->findSense("darkvision")->range == doctest::Approx(120.0));
  CHECK(creature->passivePerception() == 18);

  const auto walk = engine.movementSpeed(*creature, "walk", "land");
  REQUIRE(walk.has_value());
  CHECK(walk->baseSpeed == doctest::Approx(40.0));
}

TEST_CASE("movement: the shipped D&D ruleset exposes creature speed and senses") {
  RulesetEngine dnd;
  REQUIRE(dnd.loadRulesetFromFile(rulesetPath("dnd5e_srd.json")));
  auto dragon = dnd.createCreature("young_black_dragon", rpg_os::Variance::Average);
  REQUIRE(dragon != nullptr);

  const auto walk = dnd.movementSpeed(*dragon, "walk", "land");
  REQUIRE(walk.has_value());
  CHECK(walk->baseSpeed == doctest::Approx(40.0));
  CHECK(walk->speed == doctest::Approx(40.0));

  CHECK(dragon->hasMovementSpeed("fly"));
  CHECK(dragon->movementSpeed("fly") == doctest::Approx(80.0));
  CHECK(dragon->hasMovementSpeed("swim"));
  CHECK_FALSE(dragon->hasMovementSpeed("burrow"));

  CHECK(dragon->hasSense("blindsight"));
  CHECK(dragon->findSense("darkvision")->range == doctest::Approx(120.0));
  CHECK_FALSE(dragon->hasSense("tremorsense"));
  CHECK(dragon->passivePerception() == 0); // the stat block lists no passive Perception

  auto ancient = dnd.createCreature("ancient_black_dragon", rpg_os::Variance::Average);
  REQUIRE(ancient != nullptr);
  CHECK(ancient->passivePerception() == 26);
  CHECK(ancient->hasCapability("darkvision"));

  auto elemental = dnd.createCreature("air_elemental", rpg_os::Variance::Average);
  REQUIRE(elemental != nullptr);
  const auto &speeds = elemental->movementSpeeds();
  const auto it = speeds.find("fly");
  REQUIRE(it != speeds.end());
  CHECK(it->second.hover);
}

TEST_CASE("movement: a creature mode the ruleset does not declare is queryable") {
  // The Dark Eye ruleset declares walk/swim/climb (no fly), but the irrhalk
  // bestiary entry has an air speed. The engine synthesizes the missing mode
  // so the creature's own speed is still reported.
  RulesetEngine tde;
  REQUIRE(tde.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  auto irrhalk = tde.createCreature("irrhalk", rpg_os::Variance::Average);
  REQUIRE(irrhalk != nullptr);
  CHECK(irrhalk->hasMovementSpeed("fly"));

  const auto fly = tde.movementSpeed(*irrhalk, "fly", "land");
  REQUIRE(fly.has_value());
  CHECK(fly->modeId == "fly");
  CHECK(fly->baseSpeed == doctest::Approx(36.0));
  CHECK(fly->speed == doctest::Approx(36.0));

  // The creature's own modes are listed even though fly is not a rule mode.
  const auto status = tde.movementStatus(*irrhalk, "land");
  REQUIRE(status.has_value());
  const auto it = std::find_if(status->options.begin(), status->options.end(),
                               [](const MovementOption &o) { return o.modeId == "fly"; });
  REQUIRE(it != status->options.end());
  CHECK(it->baseSpeed == doctest::Approx(36.0));
}
