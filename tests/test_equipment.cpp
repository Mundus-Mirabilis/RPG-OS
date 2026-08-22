// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_equipment.cpp
 * @brief Equipment slots, equipping/unequipping, and gear-modified effective stats.
 */
#include "test_fixtures.hpp"

TEST_CASE("bookkeeping: Equipment slots hold one item each (phase 0)") {
  rpg_os::Equipment eq;
  CHECK(eq.equip("weapon_hand", "sword").empty());
  CHECK(eq.isEquipped("weapon_hand"));
  CHECK(eq.itemIn("weapon_hand") == "sword");
  CHECK(eq.equip("weapon_hand", "axe") == "sword"); // overwrite returns old
  CHECK(eq.itemIn("weapon_hand") == "axe");
  CHECK(eq.unequip("weapon_hand") == "axe");
  CHECK_FALSE(eq.isEquipped("weapon_hand"));
  CHECK(eq.unequip("weapon_hand").empty());
}

TEST_CASE("bookkeeping: equip and unequip move items between slots (phase 2)") {
  RulesetEngine tde;
  REQUIRE(tde.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  auto sheet = tde.createEntity("geron");
  REQUIRE(sheet != nullptr);
  // geron starts with a longsword equipped
  CHECK(sheet->equipment().isEquipped("weapon_hand"));
  CHECK(sheet->equipment().itemIn("weapon_hand") == "longsword");

  // cannot equip what you do not own
  CHECK(tde.equip(*sheet, "weapon_hand", "dagger").error() == BookkeepingError::ItemNotOwned);

  REQUIRE(tde.addItem(*sheet, "dagger", 1).has_value());
  const auto equipped = tde.equip(*sheet, "weapon_hand", "dagger");
  REQUIRE(equipped.has_value());
  CHECK(equipped->previousItemId == "longsword"); // swap
  CHECK(sheet->equipment().itemIn("weapon_hand") == "dagger");
  CHECK(sheet->inventory().count("longsword") == 1); // old item returned
  CHECK(sheet->inventory().count("dagger") == 0);

  // unequip returns the item
  REQUIRE(tde.unequip(*sheet, "weapon_hand").has_value());
  CHECK_FALSE(sheet->equipment().isEquipped("weapon_hand"));
  CHECK(sheet->inventory().count("dagger") == 1);

  // slot / item mismatches fail
  CHECK(tde.equip(*sheet, "weapon_hand", "leather_armor").error() ==
        BookkeepingError::SlotMismatch);
  CHECK(tde.equip(*sheet, "no_such_slot", "dagger").error() == BookkeepingError::SlotMismatch);
  CHECK(tde.unequip(*sheet, "weapon_hand").error() == BookkeepingError::NotEquipped);
}

TEST_CASE("bookkeeping: equipped gear modifies effective stats (phase 2)") {
  RulesetEngine tde;
  REQUIRE(tde.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  auto sheet = tde.createEntity("geron");
  REQUIRE(sheet != nullptr);
  // geron base Armor_Rating = 2; leather armor adds +3
  CHECK(sheet->baseAttribute("Armor_Rating") == 2);
  CHECK(sheet->getStat("Armor_Rating") == 2); // raw stays raw
  CHECK(sheet->getEffectiveStat("Armor_Rating") == 5);

  REQUIRE(tde.unequip(*sheet, "body_armor").has_value());
  CHECK(sheet->getEffectiveStat("Armor_Rating") == 2);
  REQUIRE(tde.equip(*sheet, "body_armor", "leather_armor").has_value());
  CHECK(sheet->getEffectiveStat("Armor_Rating") == 5);
}

TEST_CASE("bookkeeping: effective armour reduces damage in the pipeline") {
  RulesetEngine tde;
  REQUIRE(tde.loadRulesetFromFile(rulesetPath("tde5e_core.json")));
  auto geron = tde.createEntity("geron");
  REQUIRE(geron != nullptr);
  rpg_os::DynamicEntity orc(tde.ruleset(), "orc");

  // geron wears leather armour: raw Armor_Rating 2, effective 5.
  CHECK(geron->baseAttribute("Armor_Rating") == 2);
  CHECK(geron->getEffectiveStat("Armor_Rating") == 5);
  const int32_t lpBefore = geron->resource("LP");
  CHECK(tde.applyDamage(orc, *geron, "LP", 10) == -5); // 10 - effective 5

  // without the armour only the raw rating applies
  REQUIRE(tde.unequip(*geron, "body_armor").has_value());
  const int32_t lpAfter = geron->resource("LP");
  CHECK(tde.applyDamage(orc, *geron, "LP", 10) == -8); // 10 - raw 2
  CHECK(geron->resource("LP") == lpBefore - 5 - 8);
  CHECK(geron->resource("LP") == lpAfter - 8);
}

TEST_CASE("bookkeeping: an equipped item's formula modifier resolves against the wearer") {
  // A gear modifier whose `value` is a *formula* (D&D medium armour caps the
  // Dexterity contribution): AC = 10 + min(DEX_mod, 2) + 4.
  const std::string rulesetJson = R"JSON({
    "schema_version": 1, "ruleset_id": "mini", "licence": "test",
    "attributes": [
      {"id": "DEX", "name": "Dexterity", "min": 1, "max": 30, "default": 10}
    ],
    "derived_stats": [
      {"id": "DEX_mod", "name": "Dexterity Modifier", "formula": "floor((DEX - 10) / 2)"},
      {"id": "AC", "name": "Armor Class", "formula": "10 + DEX_mod"}
    ],
    "data": {
      "items": [
        {"id": "scale_mail", "name": "Scale Mail", "slot": "body_armor",
         "modifiers": [{"target_stat": "AC", "type": "override",
                        "value": "10 + min(DEX_mod, 2) + 4"}]}
      ]
    }
  })JSON";
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(rulesetJson));

  // DEX 18 -> DEX_mod +4, capped at +2 by the medium-armour formula: 16.
  DynamicEntity hero(engine.ruleset(), "hero");
  hero.setBaseAttribute("DEX", 18);
  (void)hero.equipment().equip("body_armor", "scale_mail");
  CHECK(hero.getEffectiveStat("AC") == 16);

  // DEX 12 -> DEX_mod +1, no cap hit: 10 + 1 + 4 = 15.
  DynamicEntity rogue(engine.ruleset(), "rogue");
  rogue.setBaseAttribute("DEX", 12);
  (void)rogue.equipment().equip("body_armor", "scale_mail");
  CHECK(rogue.getEffectiveStat("AC") == 15);

  // A plain numeric modifier keeps working alongside formula values.
  const std::string numericJson = R"JSON({
    "schema_version": 1, "ruleset_id": "mini2", "licence": "test",
    "attributes": [
      {"id": "DEX", "name": "Dexterity", "min": 1, "max": 30, "default": 10}
    ],
    "derived_stats": [
      {"id": "DEX_mod", "name": "Dexterity Modifier", "formula": "floor((DEX - 10) / 2)"},
      {"id": "AC", "name": "Armor Class", "formula": "10 + DEX_mod"}
    ],
    "data": {
      "items": [
        {"id": "shield", "name": "Shield", "slot": "shield",
         "modifiers": [{"target_stat": "AC", "type": "add", "value": 2}]}
      ]
    }
  })JSON";
  RulesetEngine engine2;
  REQUIRE(engine2.loadRulesetFromJson(numericJson));
  DynamicEntity knight(engine2.ruleset(), "knight");
  knight.setBaseAttribute("DEX", 14); // base AC 10 + 2 = 12
  (void)knight.equipment().equip("shield", "shield");
  CHECK(knight.getEffectiveStat("AC") == 14); // +2 from the shield
}

TEST_CASE("bookkeeping: D&D armour items modify effective AC from their records") {
  RulesetEngine dnd;
  REQUIRE(dnd.loadRulesetFromFile(rulesetPath("dnd5e_srd.json")));

  // Heavy armour overrides AC outright; a shield adds on top.
  DynamicEntity fighter(dnd.ruleset(), "fighter");
  fighter.setBaseAttribute("DEX", 10);                         // DEX_mod 0 -> base AC 10
  (void)fighter.equipment().equip("body_armor", "chain_mail"); // override 16
  (void)fighter.equipment().equip("shield", "shield");         // +2
  CHECK(fighter.getEffectiveStat("AC") == 18);

  // Medium armour caps the Dexterity contribution: DEX 18 -> +4 capped at +2.
  DynamicEntity agile(dnd.ruleset(), "agile");
  agile.setBaseAttribute("DEX", 18);
  (void)agile.equipment().equip("body_armor", "scale_mail"); // 10 + min(4,2) + 4
  CHECK(agile.getEffectiveStat("AC") == 16);

  // Light armour keeps the full Dexterity bonus.
  DynamicEntity skirmisher(dnd.ruleset(), "skirmisher");
  skirmisher.setBaseAttribute("DEX", 18);
  (void)skirmisher.equipment().equip("body_armor", "studded_leather_armor"); // 12 + DEX_mod
  CHECK(skirmisher.getEffectiveStat("AC") == 16);
}
