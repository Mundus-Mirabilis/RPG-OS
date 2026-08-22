// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_inventory.cpp
 * @brief Inventory stacks, nested containers, and carrying weight.
 */
#include "test_fixtures.hpp"

TEST_CASE("bookkeeping: Inventory adds, merges, and removes (phase 0)") {
  rpg_os::Inventory inv;
  inv.add(ItemInstance{"sword", 1, {}});
  inv.add(ItemInstance{"sword", 2, {}});
  CHECK(inv.count("sword") == 3);
  inv.add(ItemInstance{"potion", 5, {}});
  CHECK(inv.size() == 2);
  CHECK(inv.remove("sword", 2));
  CHECK(inv.count("sword") == 1);
  CHECK_FALSE(inv.remove("sword", 5)); // not enough -> unchanged
  CHECK(inv.count("sword") == 1);
  CHECK(inv.remove("sword", 1));
  CHECK_FALSE(inv.has("sword"));
}

TEST_CASE("bookkeeping: D&D encumbrance from weight and capacity (phase 2)") {
  RulesetEngine dnd;
  REQUIRE(dnd.loadRulesetFromFile(rulesetPath("dnd5e_srd.json")));
  const auto &ruleset = dnd.ruleset();
  DynamicEntity sheet(ruleset, "test");
  sheet.setBaseAttribute("STR", 10); // capacity = 15 * 10 = 150 lb

  REQUIRE(dnd.addItem(sheet, "club", 10).has_value()); // 10 * 2 lb = 20 lb
  CHECK(dnd.carriedWeight(sheet).value == doctest::Approx(20.0));
  const auto capacity = dnd.carryingCapacity(sheet);
  REQUIRE(capacity.has_value());
  CHECK(capacity->value == doctest::Approx(150.0));
  const auto level = dnd.encumbranceLevel(sheet);
  REQUIRE(level.has_value());
  CHECK(*level == 0);

  REQUIRE(dnd.addItem(sheet, "club", 70).has_value()); // 160 lb total
  const auto heavy = dnd.encumbranceLevel(sheet);
  REQUIRE(heavy.has_value());
  CHECK(*heavy == 2); // ratio 160/150 > 1.0 -> heaviest level
}

TEST_CASE("bookkeeping: containers hold items (bag-in-bags)") {
  const std::string rulesetJson = R"({
    "schema_version": 1, "ruleset_id": "mini", "licence": "test",
    "attributes": [{"id": "STR", "name": "Strength", "min": 1, "max": 30, "default": 10}],
    "encumbrance": {"weight_unit": "lb", "default_weight": 0.0, "capacity": "15 * STR", "levels": []},
    "data": {
      "items": [
        {"id": "backpack", "name": "Backpack", "weight": "5 lb."},
        {"id": "club", "name": "Club", "weight": "2 lb."}
      ]
    }
  })";
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(rulesetJson));
  const auto &ruleset = engine.ruleset();
  DynamicEntity sheet(ruleset, "test");
  sheet.setBaseAttribute("STR", 10);

  REQUIRE(engine.addItem(sheet, "backpack", 1).has_value());
  REQUIRE(engine.addItem(sheet, "club", 2).has_value());
  // flat count does not see what is inside the container...
  CHECK(sheet.inventory().count("club") == 2);
  CHECK(sheet.inventory().totalCount("club") == 2);
  CHECK(engine.carriedWeight(sheet).value == doctest::Approx(9.0)); // 5 + 2*2

  // ...until we put a club into the backpack.
  REQUIRE(engine.addItemToContainer(sheet, "backpack", "club", 1).has_value());
  CHECK(sheet.inventory().countIn("backpack", "club") == 1);
  CHECK(sheet.inventory().totalCount("club") == 3);
  CHECK(engine.carriedWeight(sheet).value == doctest::Approx(11.0)); // +2 in the pack
  REQUIRE(sheet.inventory().contentsOf("backpack") != nullptr);
  CHECK(sheet.inventory().contentsOf("backpack")->size() == 1);

  // a missing container is a distinct error
  CHECK(engine.addItemToContainer(sheet, "nope", "club", 1).error() ==
        BookkeepingError::UnknownContainer);

  // removing from the container returns the count to flat-only
  REQUIRE(engine.removeItemFromContainer(sheet, "backpack", "club", 1).has_value());
  CHECK(sheet.inventory().countIn("backpack", "club") == 0);
  CHECK(sheet.inventory().totalCount("club") == 2);
  CHECK(engine.carriedWeight(sheet).value == doctest::Approx(9.0));

  // serialization round-trips nested contents
  REQUIRE(engine.addItemToContainer(sheet, "backpack", "club", 1).has_value());
  rpg_os::Json saved;
  sheet.toJson(saved);
  DynamicEntity restored(ruleset, "test");
  restored.fromJson(saved);
  CHECK(restored.inventory().countIn("backpack", "club") == 1);
  CHECK(restored.inventory().totalCount("club") == 3);
}

TEST_CASE("bookkeeping: carried load reports weight, size, and item count") {
  const std::string rulesetJson = R"({
    "schema_version": 1, "ruleset_id": "mini", "licence": "test",
    "attributes": [{"id": "STR", "name": "Strength", "min": 1, "max": 30, "default": 10}],
    "encumbrance": {
      "weight_unit": "lb", "default_weight": 1.0, "capacity": "15 * STR",
      "size_unit": "slot", "default_item_size": 1.0, "size_capacity": "10",
      "item_capacity": "5", "levels": []
    },
    "data": {
      "items": [
        {"id": "heavy_box", "name": "Heavy Box", "weight": "20 lb.", "size": 3},
        {"id": "light_box", "name": "Light Box"}
      ]
    }
  })";
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(rulesetJson));
  const auto &ruleset = engine.ruleset();
  DynamicEntity sheet(ruleset, "test");
  sheet.setBaseAttribute("STR", 10);

  auto load = engine.carriedLoad(sheet);
  CHECK(load.weight == doctest::Approx(0.0));
  CHECK(load.size == doctest::Approx(0.0));
  CHECK(load.items == 0);

  REQUIRE(engine.addItem(sheet, "heavy_box", 1).has_value());
  REQUIRE(engine.addItem(sheet, "light_box", 2).has_value());
  load = engine.carriedLoad(sheet);
  CHECK(load.weight == doctest::Approx(22.0)); // 20 + 2 * 1
  CHECK(load.size == doctest::Approx(5.0));    // 3 + 2 * 1 (default size 1)
  CHECK(load.items == 3);

  const auto cap = engine.capacity(sheet);
  CHECK(cap.enabled);
  CHECK(cap.weight == doctest::Approx(150.0)); // 15 * 10
  CHECK(cap.size == doctest::Approx(10.0));
  CHECK(cap.items == 5);

  const auto status = engine.inventoryStatus(sheet);
  CHECK_FALSE(status.full());
  CHECK_FALSE(status.overWeight);
  CHECK_FALSE(status.overSize);
  CHECK_FALSE(status.overItems);
}

TEST_CASE("bookkeeping: canCarry rejects when any axis overflows") {
  const std::string rulesetJson = R"({
    "schema_version": 1, "ruleset_id": "mini", "licence": "test",
    "attributes": [{"id": "STR", "name": "Strength", "min": 1, "max": 30, "default": 10}],
    "encumbrance": {
      "weight_unit": "lb", "default_weight": 1.0, "capacity": "15 * STR",
      "size_unit": "slot", "default_item_size": 1.0, "size_capacity": "10",
      "item_capacity": "5", "levels": []
    },
    "data": {
      "items": [
        {"id": "heavy_box", "name": "Heavy Box", "weight": "20 lb.", "size": 3},
        {"id": "light_box", "name": "Light Box", "weight": "1 lb.", "size": 1}
      ]
    }
  })";
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(rulesetJson));
  const auto &ruleset = engine.ruleset();
  DynamicEntity sheet(ruleset, "test");
  sheet.setBaseAttribute("STR", 10);

  // 7 heavy boxes: weight 140 lb <= 150, but size 21 > 10 -> size overflow.
  auto ok = engine.canCarry(sheet, "heavy_box", 7);
  REQUIRE(ok.has_value());
  CHECK_FALSE(*ok);
  // 3 heavy boxes fit on every axis.
  ok = engine.canCarry(sheet, "heavy_box", 3);
  CHECK(*ok);
  // 5 light boxes = exactly the item limit (still allowed).
  ok = engine.canCarry(sheet, "light_box", 5);
  CHECK(*ok);
  // 6 light boxes -> item-count overflow.
  ok = engine.canCarry(sheet, "light_box", 6);
  CHECK_FALSE(*ok);
  // unknown item -> UnknownItem
  CHECK(engine.canCarry(sheet, "nope", 1).error() == BookkeepingError::UnknownItem);
}

TEST_CASE("bookkeeping: inventoryStatus reports the over-limit axis") {
  const std::string rulesetJson = R"({
    "schema_version": 1, "ruleset_id": "mini", "licence": "test",
    "attributes": [{"id": "STR", "name": "Strength", "min": 1, "max": 30, "default": 10}],
    "encumbrance": {
      "weight_unit": "lb", "default_weight": 1.0, "capacity": "15 * STR",
      "size_unit": "slot", "default_item_size": 1.0, "size_capacity": "10",
      "item_capacity": "5", "levels": []
    },
    "data": {
      "items": [{"id": "light_box", "name": "Light Box", "weight": "1 lb.", "size": 1}]
    }
  })";
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(rulesetJson));
  const auto &ruleset = engine.ruleset();
  DynamicEntity sheet(ruleset, "test");
  sheet.setBaseAttribute("STR", 10);

  for (int i = 0; i < 6; ++i) {
    REQUIRE(engine.addItem(sheet, "light_box", 1).has_value());
  }
  const auto status = engine.inventoryStatus(sheet);
  CHECK(status.carried.items == 6);
  CHECK(status.overItems);
  CHECK(status.full());
  // a sheet over its item limit cannot add more boxes
  CHECK_FALSE(*engine.canCarry(sheet, "light_box", 1));
}

TEST_CASE("bookkeeping: containers have their own capacity (weight/size/count)") {
  const std::string rulesetJson = R"({
    "schema_version": 1, "ruleset_id": "mini", "licence": "test",
    "attributes": [{"id": "STR", "name": "Strength", "min": 1, "max": 30, "default": 10}],
    "encumbrance": {
      "weight_unit": "lb", "default_weight": 1.0, "capacity": "15 * STR",
      "size_unit": "slot", "default_item_size": 1.0, "levels": []
    },
    "data": {
      "items": [
        {"id": "backpack", "name": "Backpack", "weight": "5 lb.", "size": 1,
         "capacity": {"weight": 30, "size": 6, "items": 5}},
        {"id": "club", "name": "Club", "weight": "2 lb.", "size": 1}
      ]
    }
  })";
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(rulesetJson));
  const auto &ruleset = engine.ruleset();
  DynamicEntity sheet(ruleset, "test");
  sheet.setBaseAttribute("STR", 10);

  const auto cap = engine.containerCapacity("backpack");
  REQUIRE(cap.has_value());
  CHECK(cap->weight == doctest::Approx(30.0));
  CHECK(cap->size == doctest::Approx(6.0));
  CHECK(cap->items == 5);
  CHECK(cap->enabled);
  CHECK(engine.containerCapacity("nope").error() == BookkeepingError::UnknownItem);
  // a non-container item declares no capacity
  const auto clubCap = engine.containerCapacity("club");
  REQUIRE(clubCap.has_value());
  CHECK_FALSE(clubCap->enabled);

  REQUIRE(engine.addItem(sheet, "backpack", 1).has_value());
  // 16 clubs = 32 lb > 30 weight -> reject
  CHECK_FALSE(*engine.canAddToContainer(sheet, "backpack", "club", 16));
  // 7 clubs = size 7 > 6 -> reject
  CHECK_FALSE(*engine.canAddToContainer(sheet, "backpack", "club", 7));
  // 6 clubs = 12 lb, size 6, items 6 > 5 -> reject by count
  CHECK_FALSE(*engine.canAddToContainer(sheet, "backpack", "club", 6));
  // 5 clubs fit exactly on every axis
  CHECK(*engine.canAddToContainer(sheet, "backpack", "club", 5));
  REQUIRE(engine.addItemToContainer(sheet, "backpack", "club", 5).has_value());
  // the container is now full: one more club cannot fit
  CHECK_FALSE(*engine.canAddToContainer(sheet, "backpack", "club", 1));
  // ...and adding it is refused with OverCapacity
  CHECK(engine.addItemToContainer(sheet, "backpack", "club", 1).error() ==
        BookkeepingError::OverCapacity);
  CHECK(sheet.inventory().countIn("backpack", "club") == 5);
  // containerLoad reports what is inside
  const auto cl = engine.containerLoad(sheet, "backpack");
  CHECK(cl.items == 5);
  CHECK(cl.weight == doctest::Approx(10.0));
  CHECK(cl.size == doctest::Approx(5.0));
}

TEST_CASE("bookkeeping: no encumbrance rules means unlimited carrying") {
  const std::string rulesetJson = R"({
    "schema_version": 1, "ruleset_id": "mini", "licence": "test",
    "attributes": [{"id": "STR", "name": "Strength", "min": 1, "max": 30, "default": 10}],
    "data": {"items": [{"id": "box", "name": "Box", "weight": "10 lb."}]}
  })";
  RulesetEngine engine;
  REQUIRE(engine.loadRulesetFromJson(rulesetJson));
  const auto &ruleset = engine.ruleset();
  DynamicEntity sheet(ruleset, "test");
  sheet.setBaseAttribute("STR", 10);

  const auto cap = engine.capacity(sheet);
  CHECK_FALSE(cap.enabled);
  const auto status = engine.inventoryStatus(sheet);
  CHECK_FALSE(status.full());
  CHECK(*engine.canCarry(sheet, "box", 1000));
}
