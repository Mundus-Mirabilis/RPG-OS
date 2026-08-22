// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file game_session.hpp
 * @ingroup rpg_os_universal
 * @brief The application-facing facade ("OS kernel").
 *
 * An application that drives a tabletop session should talk to one object:
 * a @ref GameSession. It owns a @ref RulesetEngine (the interpreter), the
 * character / creature sheets it created, and the minimal shared @ref
 * WorldState (party treasury, day counter). The domain operations — economy,
 * equipment, conditions, magic, advancement, rests, afflictions — are exposed
 * here as conveniences over the engine, so application code rarely needs to
 * thread engine and entity pointers around.
 *
 * @par Why a facade instead of extending RulesetEngine?
 * The engine is about *resolution* (load rules, create entities, resolve
 * checks and damage). A session is about *running a game*: it owns the
 * characters and the shared bookkeeping. Keeping the two separate lets an
 * application that only needs the rules (a character builder, a test harness)
 * use the engine alone, and lets the session grow world-agnostic bookkeeping
 * without bloating the resolver. Per the project's scope, the session keeps
 * per-character / creature / object state and only the *shared* bookkeeping
 * (treasury, day) — never plot, maps, or encounter logic.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <rpg_os/common/event_system.hpp>
#include <rpg_os/core/capacity.hpp>
#include <rpg_os/universal/engine.hpp>
#include <rpg_os/universal/entity_registry.hpp>
#include <rpg_os/universal/movement.hpp>
#include <rpg_os/universal/world_state.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rpg_os {

/// Application-facing session facade (see the file doc comment).
class GameSession {
public:
  /// Loads a ruleset file; false (with @ref lastError) on failure.
  bool loadRulesetFromFile(std::string_view path) {
    return m_engine.loadRulesetFromFile(path);
  }
  /// Loads a ruleset from a JSON string; false (with @ref lastError) on failure.
  bool loadRulesetFromJson(std::string_view jsonContent) {
    return m_engine.loadRulesetFromJson(jsonContent);
  }

  /// Whether a ruleset is loaded.
  [[nodiscard]] bool loaded() const noexcept {
    return m_engine.loaded();
  }
  /// Message from the last failed load.
  [[nodiscard]] const std::string &lastError() const noexcept {
    return m_engine.lastError();
  }

  /// The underlying engine (resolution primitives).
  [[nodiscard]] RulesetEngine &engine() noexcept {
    return m_engine;
  }
  [[nodiscard]] const RulesetEngine &engine() const noexcept {
    return m_engine;
  }

  /// Creates a character sheet from an archetype and takes ownership of it.
  /// The returned reference stays valid (the sheet lives on the heap).
  [[nodiscard]] DynamicEntity &createCharacter(std::string_view archetypeId) {
    return adopt(m_engine.createEntity(archetypeId));
  }
  /// Creates a creature sheet from a bestiary entry and takes ownership.
  [[nodiscard]] DynamicEntity &createCreature(std::string_view creatureId) {
    return adopt(m_engine.createCreature(creatureId));
  }

  /// Creates a character and returns a stable @ref EntityHandle to it (see
  /// @ref createCharacter for the owning-reference variant).
  [[nodiscard]] EntityHandle createCharacterHandle(std::string_view archetypeId) {
    return adoptHandle(m_engine.createEntity(archetypeId));
  }
  /// Creates a creature and returns a stable @ref EntityHandle to it.
  [[nodiscard]] EntityHandle createCreatureHandle(std::string_view creatureId) {
    return adoptHandle(m_engine.createCreature(creatureId));
  }

  // ---- entity registry --------------------------------------------------
  /// Looks up a live entity by its unique instance id; @c nullptr when absent.
  [[nodiscard]] DynamicEntity *findEntity(EntityId id) const {
    return m_registry.get(id);
  }
  /// A non-owning handle to the entity with `id` (null handle when absent).
  [[nodiscard]] EntityHandle entity(EntityId id) const {
    return m_registry.handle(id);
  }
  /// Removes an entity from the session (destroying the last owner).
  [[nodiscard]] bool removeEntity(EntityId id) {
    return m_registry.remove(id);
  }
  /// Number of entities the session currently owns.
  [[nodiscard]] std::size_t entityCount() const noexcept {
    return m_registry.size();
  }
  /// All live entities, in unspecified order.
  [[nodiscard]] std::vector<DynamicEntity *> entities() const {
    return m_registry.all();
  }

  /// The shared world bookkeeping (treasury, day counter).
  [[nodiscard]] WorldState &world() noexcept {
    return m_world;
  }
  [[nodiscard]] const WorldState &world() const noexcept {
    return m_world;
  }

  /// Registers a listener for a bookkeeping event (e.g. OnCurrencyChanged).
  [[nodiscard]] uint64_t onEvent(EventType type, EventBus::Callback callback) {
    return m_engine.registerEventListener(type, std::move(callback));
  }

  // ---- economy ---------------------------------------------------------
  [[nodiscard]] bool hasCurrency() const noexcept {
    return m_engine.hasCurrency();
  }
  [[nodiscard]] const CurrencySystem &currencySystem() const noexcept {
    return m_engine.currencySystem();
  }
  [[nodiscard]] std::expected<Money, BookkeepingError> itemPrice(std::string_view itemId) const {
    return m_engine.itemPrice(itemId);
  }
  [[nodiscard]] std::expected<void, BookkeepingError>
  addItem(DynamicEntity &sheet, std::string_view itemId, int32_t quantity = 1) {
    return m_engine.addItem(sheet, itemId, quantity);
  }
  [[nodiscard]] std::expected<void, BookkeepingError>
  removeItem(DynamicEntity &sheet, std::string_view itemId, int32_t quantity = 1) {
    return m_engine.removeItem(sheet, itemId, quantity);
  }
  [[nodiscard]] std::expected<void, BookkeepingError>
  addItemToContainer(DynamicEntity &sheet, std::string_view containerItemId,
                     std::string_view itemId, int32_t quantity = 1) {
    return m_engine.addItemToContainer(sheet, containerItemId, itemId, quantity);
  }
  [[nodiscard]] std::expected<void, BookkeepingError>
  removeItemFromContainer(DynamicEntity &sheet, std::string_view containerItemId,
                          std::string_view itemId, int32_t quantity = 1) {
    return m_engine.removeItemFromContainer(sheet, containerItemId, itemId, quantity);
  }
  [[nodiscard]] std::expected<void, BookkeepingError> pay(DynamicEntity &from, DynamicEntity *to,
                                                          Money amount) {
    return m_engine.pay(from, to, amount);
  }
  [[nodiscard]] std::expected<Money, BookkeepingError>
  buy(DynamicEntity &buyer, DynamicEntity *seller, std::string_view itemId, int32_t quantity = 1) {
    return m_engine.buy(buyer, seller, itemId, quantity);
  }

  // ---- equipment & encumbrance -----------------------------------------
  [[nodiscard]] std::expected<RulesetEngine::EquipResult, BookkeepingError>
  equip(DynamicEntity &sheet, std::string_view slotId, std::string_view itemId) {
    return m_engine.equip(sheet, slotId, itemId);
  }
  [[nodiscard]] std::expected<void, BookkeepingError> unequip(DynamicEntity &sheet,
                                                              std::string_view slotId) {
    return m_engine.unequip(sheet, slotId);
  }
  [[nodiscard]] Weight carriedWeight(const DynamicEntity &sheet) const {
    return m_engine.carriedWeight(sheet);
  }
  [[nodiscard]] CarriedLoad carriedLoad(const DynamicEntity &sheet) const {
    return m_engine.carriedLoad(sheet);
  }
  [[nodiscard]] CarryingCapacity capacity(const DynamicEntity &sheet) const {
    return m_engine.capacity(sheet);
  }
  [[nodiscard]] InventoryStatus inventoryStatus(const DynamicEntity &sheet) const {
    return m_engine.inventoryStatus(sheet);
  }
  [[nodiscard]] std::expected<bool, BookkeepingError>
  canCarry(const DynamicEntity &sheet, std::string_view itemId, int32_t quantity = 1) const {
    return m_engine.canCarry(sheet, itemId, quantity);
  }
  [[nodiscard]] std::expected<CarryingCapacity, BookkeepingError>
  containerCapacity(std::string_view containerItemId) const {
    return m_engine.containerCapacity(containerItemId);
  }
  [[nodiscard]] CarriedLoad containerLoad(const DynamicEntity &sheet,
                                          std::string_view containerItemId) const {
    return m_engine.containerLoad(sheet, containerItemId);
  }
  [[nodiscard]] std::expected<bool, BookkeepingError>
  canAddToContainer(const DynamicEntity &sheet, std::string_view containerItemId,
                    std::string_view itemId, int32_t quantity = 1) const {
    return m_engine.canAddToContainer(sheet, containerItemId, itemId, quantity);
  }
  [[nodiscard]] std::expected<Weight, BookkeepingError>
  carryingCapacity(const DynamicEntity &sheet) const {
    return m_engine.carryingCapacity(sheet);
  }
  [[nodiscard]] std::expected<int32_t, BookkeepingError>
  encumbranceLevel(const DynamicEntity &sheet) const {
    return m_engine.encumbranceLevel(sheet);
  }
  [[nodiscard]] std::expected<void, BookkeepingError>
  updateEncumbrance(DynamicEntity &sheet) const {
    return m_engine.updateEncumbrance(sheet);
  }

  // ---- movement & terrain -----------------------------------------------
  [[nodiscard]] bool hasMovement() const noexcept {
    return m_engine.hasMovement();
  }
  [[nodiscard]] const MovementConfig &movementConfig() const noexcept {
    return m_engine.movementConfig();
  }
  [[nodiscard]] std::string effectiveTerrain(const DynamicEntity &sheet) const {
    return m_engine.effectiveTerrain(sheet);
  }
  [[nodiscard]] std::expected<MovementStatus, BookkeepingError>
  movementStatus(const DynamicEntity &sheet, std::string_view terrainId = {}) const {
    return m_engine.movementStatus(sheet, terrainId);
  }
  [[nodiscard]] std::expected<MovementOption, BookkeepingError>
  movementSpeed(const DynamicEntity &sheet, std::string_view modeId,
                std::string_view terrainId = {}) const {
    return m_engine.movementSpeed(sheet, modeId, terrainId);
  }
  [[nodiscard]] std::expected<double, BookkeepingError>
  movementCost(const DynamicEntity &sheet, std::string_view modeId, double distance,
               std::string_view terrainId = {}) const {
    return m_engine.movementCost(sheet, modeId, distance, terrainId);
  }
  [[nodiscard]] bool canRegenerate(const DynamicEntity &sheet,
                                   std::string_view terrainId = {}) const {
    return m_engine.canRegenerate(sheet, terrainId);
  }
  [[nodiscard]] std::expected<MovementOutcome, BookkeepingError>
  move(DynamicEntity &sheet, std::string_view modeId, double distance,
       std::string_view terrainId = {}) const {
    return m_engine.move(sheet, modeId, distance, terrainId);
  }

  // ---- conditions & effects ---------------------------------------------
  void applyCondition(DynamicEntity &sheet, std::string_view conditionId, int32_t stacks = 1,
                      int32_t duration = 0, std::string_view source = {}) {
    m_engine.applyCondition(sheet, conditionId, stacks, duration, source);
  }
  void removeCondition(DynamicEntity &sheet, std::string_view conditionId) {
    m_engine.removeCondition(sheet, conditionId);
  }
  [[nodiscard]] int32_t tickEffects(DynamicEntity &sheet) {
    return m_engine.tickEffects(sheet);
  }
  void runTurn(DynamicEntity &sheet) {
    m_engine.runTurn(sheet);
  }

  // ---- time --------------------------------------------------------------
  /// Advances game time by `days`: increments the world day counter, fires
  /// @c OnTimePassed (elapsed + new day in the payload), and ticks every owned
  /// sheet's effect durations once, so day-scale durations decay and expired
  /// effects are stripped. Returns the new day.
  int32_t advanceTime(int32_t days = 1) {
    m_world.advanceDays(days);
    const int32_t newDay = m_world.day;
    m_engine.announceTimePassed(days, newDay);
    for (DynamicEntity *sheet : m_registry.all()) {
      (void)m_engine.tickEffects(*sheet);
    }
    return newDay;
  }

  // ---- save / load ------------------------------------------------------
  /// Snapshots the whole session — every owned sheet (via @c toJson) plus the
  /// shared @ref WorldState — into one JSON save state. Per-instance entity
  /// ids are preserved, so @ref loadState restores the same living characters.
  [[nodiscard]] Json saveState() const {
    Json out = Json::object();
    Json worldJson;
    m_world.toJson(worldJson);
    out["world"] = std::move(worldJson);
    Json sheets = Json::array();
    for (const DynamicEntity *sheet : m_registry.all()) {
      Json sheetJson;
      sheet->toJson(sheetJson);
      sheets.push_back(std::move(sheetJson));
    }
    out["sheets"] = sheets;
    return out;
  }

  /// Restores a state produced by @c saveState: replaces every owned sheet
  /// and the shared world. Each sheet is recreated from its archetype /
  /// creature record (so resource-pool bounds are rebuilt from the ruleset)
  /// and its saved living state — current pools, conditions with remaining
  /// durations, inventory, spell slots, effect timeline — is then applied,
  /// with events suppressed during the restore. Returns how many sheets were
  /// restored (a sheet whose type is absent from the ruleset is skipped).
  std::size_t loadState(const Json &in) {
    if (!in.is_object()) {
      return 0;
    }
    m_registry.clear();
    if (in.contains("world") && in.at("world").is_object()) {
      m_world.fromJson(in.at("world"));
    }
    if (!in.contains("sheets") || !in.at("sheets").is_array()) {
      return 0;
    }
    std::size_t restored = 0;
    for (const Json &sheetJson : in.at("sheets")) {
      if (!sheetJson.is_object()) {
        continue;
      }
      const std::string typeId = sheetJson.value("id", "");
      if (typeId.empty()) {
        continue;
      }
      std::shared_ptr<DynamicEntity> sheet = m_engine.createEntity(typeId);
      if (sheet == nullptr) {
        sheet = m_engine.createCreature(typeId);
      }
      if (sheet == nullptr) {
        continue; // type not present in this ruleset
      }
      sheet->setEventsSuppressed(true);
      sheet->fromJson(sheetJson);
      sheet->setEventsSuppressed(false);
      (void)m_registry.add(std::move(sheet));
      ++restored;
    }
    return restored;
  }

  // ---- magic ------------------------------------------------------------
  [[nodiscard]] bool prepareSpell(DynamicEntity &sheet, std::string_view spellId) const {
    return m_engine.prepareSpell(sheet, spellId);
  }
  [[nodiscard]] int32_t spellSlotsRemaining(const DynamicEntity &sheet, int32_t level) const {
    return m_engine.spellSlotsRemaining(sheet, level);
  }
  [[nodiscard]] std::expected<void, BookkeepingError> spendSpellSlot(DynamicEntity &sheet,
                                                                     int32_t level) {
    return m_engine.spendSpellSlot(sheet, level);
  }
  void recoverSpellSlots(DynamicEntity &sheet) {
    m_engine.recoverSpellSlots(sheet);
  }
  template <RandomNumberGenerator Rng>
  [[nodiscard]] RulesetEngine::SpellResult
  castSpellPrepared(std::string_view spellId, DynamicEntity &actor, DynamicEntity *target,
                    const CheckParams &params, Rng &rng) {
    return m_engine.castSpellPrepared(spellId, actor, target, params, rng);
  }
  [[nodiscard]] RulesetEngine::SpellResult castSpellPrepared(std::string_view spellId,
                                                             DynamicEntity &actor,
                                                             DynamicEntity *target,
                                                             const CheckParams &params) {
    return m_engine.castSpellPrepared(spellId, actor, target, params);
  }

  // ---- advancement, rest, afflictions -----------------------------------
  [[nodiscard]] std::expected<LevelUp, BookkeepingError> gainXp(DynamicEntity &sheet, int64_t xp) {
    return m_engine.gainXp(sheet, xp);
  }
  [[nodiscard]] std::expected<int32_t, BookkeepingError>
  improvementCost(std::string_view costTableId, int32_t currentRating) const {
    return m_engine.improvementCost(costTableId, currentRating);
  }
  void shortRest(DynamicEntity &sheet) {
    m_engine.shortRest(sheet);
  }
  void longRest(DynamicEntity &sheet) {
    m_engine.longRest(sheet);
  }
  template <RandomNumberGenerator Rng>
  [[nodiscard]] RulesetEngine::AfflictionResult
  applyCurse(std::string_view curseId, DynamicEntity &victim, const CheckParams &params, Rng &rng) {
    return m_engine.applyCurse(curseId, victim, params, rng);
  }
  [[nodiscard]] bool cureAffliction(DynamicEntity &victim, std::string_view section,
                                    std::string_view id) {
    return m_engine.cureAffliction(victim, section, id);
  }

private:
  /// Takes ownership of a freshly created sheet; returns the reference.
  [[nodiscard]] DynamicEntity &adopt(std::shared_ptr<DynamicEntity> sheet) {
    DynamicEntity *raw = sheet.get();
    (void)m_registry.add(std::move(sheet));
    return *raw;
  }
  /// Takes ownership of a freshly created sheet and returns a stable handle.
  [[nodiscard]] EntityHandle adoptHandle(std::shared_ptr<DynamicEntity> sheet) {
    const EntityId id = m_registry.add(std::move(sheet));
    return m_registry.handle(id);
  }

  RulesetEngine m_engine;
  EntityRegistry m_registry;
  WorldState m_world;
};

} // namespace rpg_os
