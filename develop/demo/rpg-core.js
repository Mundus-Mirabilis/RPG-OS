// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * The shared WASM wrapper for the RPG-OS engine — the implementation used by
 * BOTH the npm package (wasm/package/index.mjs, which copies this file to
 * core.mjs at build time) and the static browser demo (web/demo/rpg-browser.js).
 *
 * It is a thin ergonomic wrapper around the flat C API of the compiled
 * engine: it allocates strings and reads results from WASM memory, wraps the
 * entity / combatant-spec / engine handles in classes, and throws on engine
 * errors. It deliberately does not import the emscripten glue — the module
 * path differs between the npm package (`./dist/`) and the demo (`./wasm/`) —
 * so each wrapper imports its own glue and provides `init()`.
 */
// ---- low-level marshaling over the flat C API ------------------------------

/** Writes `str` into WASM memory; returns `{ ptr, len }` (caller frees ptr). */
function alloc(m, str) {
  const len = m.lengthBytesUTF8(str);
  const ptr = m._malloc(len + 1);
  m.stringToUTF8(str, ptr, len + 1);
  return { ptr, len };
}

function free(m, ptr) {
  if (ptr) m._free(ptr);
}

function read(m, ptr) {
  return ptr ? m.UTF8ToString(ptr) : '';
}

function jsonResult(m, fn, ...args) {
  const raw = fn(...args);
  const parsed = raw ? JSON.parse(read(m, raw)) : null;
  if (parsed && parsed.error) throw new Error(parsed.error);
  return parsed;
}

// ---- public API -------------------------------------------------------------

/** A live character sheet (wraps a C++ DynamicEntity handle). */
export class Entity {
  constructor(m, ptr, onDispose) {
    this._m = m;
    this._ptr = ptr;
    this._onDispose = onDispose;
  }

  /** Sets a base attribute or skill rating. Returns this for chaining. */
  setStat(stat, value) {
    const { ptr } = alloc(this._m, stat);
    try {
      this._m._rpg_os_entity_set_stat(this._ptr, ptr, value);
    } finally {
      free(this._m, ptr);
    }
    return this;
  }

  /** Reads a stat (attribute, skill rating, or derived stat); 0 when unknown. */
  getStat(stat) {
    const { ptr } = alloc(this._m, stat);
    try {
      return this._m._rpg_os_entity_get_stat(this._ptr, ptr);
    } finally {
      free(this._m, ptr);
    }
  }

  /** Current value of a resource pool (e.g. "LP", "HP", "AE"); 0 when absent. */
  getResource(pool) {
    const { ptr } = alloc(this._m, pool);
    try {
      return this._m._rpg_os_entity_get_resource(this._ptr, ptr);
    } finally {
      free(this._m, ptr);
    }
  }

  /** Applies a delta to a resource pool (clamped); returns the amount applied. */
  modifyResource(pool, delta) {
    const { ptr } = alloc(this._m, pool);
    try {
      return this._m._rpg_os_entity_modify_resource(this._ptr, ptr, delta);
    } finally {
      free(this._m, ptr);
    }
  }

  /** (Re)creates the resource pools from the current stats. Returns this. */
  refreshResources() {
    this._m._rpg_os_entity_refresh_resources(this._ptr);
    return this;
  }

  /** Serializes the sheet (the `DynamicEntity` save form) as a plain object. */
  toJson() {
    return JSON.parse(read(this._m, this._m._rpg_os_entity_to_json(this._ptr)));
  }

  /** Releases the underlying WASM handle. */
  dispose() {
    if (this._ptr) {
      this._m._rpg_os_entity_free(this._ptr);
      this._ptr = null;
      this._onDispose?.();
    }
  }
}

/** A static combatant description (wraps a C++ CombatantSpec handle). */
export class CombatantSpec {
  constructor(m, ptr, onDispose) {
    this._m = m;
    this._ptr = ptr;
    this._onDispose = onDispose;
  }

  /** Releases the underlying WASM handle. */
  dispose() {
    if (this._ptr) {
      this._m._rpg_os_spec_free(this._ptr);
      this._ptr = null;
      this._onDispose?.();
    }
  }
}

/** The engine: one loaded ruleset and the operations over it. */
export class RpgOs {
  constructor(m, enginePtr) {
    this._m = m;
    this._ptr = enginePtr;
    this._entities = new Set();
    this._specs = new Set();
  }

  /** Loads a ruleset from a JSON string. Returns this for chaining. */
  loadRuleset(json) {
    const { ptr, len } = alloc(this._m, json);
    try {
      const ok = this._m._rpg_os_engine_load_json(this._ptr, ptr, len);
      if (!ok) {
        const detail = read(this._m, this._m._rpg_os_engine_last_error(this._ptr));
        throw new Error(detail || 'failed to load ruleset');
      }
      return this;
    } finally {
      free(this._m, ptr);
    }
  }

  /** The loaded ruleset's metadata (attributes/skills/pools for a form). */
  meta() {
    return jsonResult(this._m, this._m._rpg_os_engine_meta, this._ptr);
  }

  /** The ruleset's named combatants (archetypes + bestiary entries). */
  entries() {
    return jsonResult(this._m, this._m._rpg_os_engine_entries, this._ptr);
  }

  /**
   * Creates an entity from a named archetype or bestiary entry.
   * `options.variance`: 0 weakest … 4 strongest (2 = average); `options.seed`
   * makes ranged values reproducible.
   */
  createEntity(id, options = {}) {
    const { ptr } = alloc(this._m, id);
    try {
      const h = this._m._rpg_os_entity_create(
        this._ptr, ptr, options.variance ?? 2, (options.seed ?? 1) >>> 0);
      if (!h) throw new Error(`unknown entity id: "${id}"`);
      return this._trackEntity(h);
    } finally {
      free(this._m, ptr);
    }
  }

  /**
   * Creates an entity from an arbitrary character sheet — a plain stats map
   * like `{ COU: 12, Attack: 12, ... }` or a full `DynamicEntity` save form
   * (`{ stats: {...}, resources: {...}, ... }`). This is how a character
   * entered into a form, with no ruleset entry, becomes a first-class entity.
   */
  createEntityFromSheet(id, stats) {
    const sheet = stats && typeof stats === 'object' && 'stats' in stats
      ? JSON.stringify(stats)
      : JSON.stringify({ id, stats: stats ?? {} });
    const idPtr = alloc(this._m, id);
    const { ptr, len } = alloc(this._m, sheet);
    try {
      const h = this._m._rpg_os_entity_create_sheet(this._ptr, idPtr.ptr, ptr, len);
      if (!h) throw new Error(`failed to create entity from sheet: "${id}"`);
      return this._trackEntity(h);
    } finally {
      free(this._m, idPtr.ptr);
      free(this._m, ptr);
    }
  }

  _trackEntity(h) {
    const entity = new Entity(this._m, h, () => this._entities.delete(h));
    this._entities.add(h);
    return entity;
  }

  /**
   * Resolves a named check for `actor` against `target` (null = solo check).
   * `options.advantage`: +1 advantage, -1 disadvantage, 0 (default) neutral.
   * Returns `{ is_success, is_critical_success, is_critical_failure, margin,
   * remaining_pool, quality_level, raw_dice }`.
   */
  check(checkType, actor, target = null, options = {}) {
    const ct = alloc(this._m, checkType);
    try {
      return jsonResult(
        this._m, this._m._rpg_os_check, this._ptr, ct.ptr,
        actor?._ptr ?? 0, target?._ptr ?? 0, options.advantage ?? 0);
    } finally {
      free(this._m, ct.ptr);
    }
  }

  /** Builds a combatant from a named archetype or bestiary entry. */
  specFromId(id, weapon = '1d6+4') {
    const idPtr = alloc(this._m, id);
    const wPtr = alloc(this._m, weapon);
    try {
      const h = this._m._rpg_os_spec_from_id(this._ptr, idPtr.ptr, wPtr.ptr);
      if (!h) throw new Error(`unknown combatant: "${id}"`);
      return this._trackSpec(h);
    } finally {
      free(this._m, idPtr.ptr);
      free(this._m, wPtr.ptr);
    }
  }

  /** Builds a combatant from a live entity (any character can fight). */
  specFromEntity(entity, weapon = '1d6+4') {
    if (!entity || !entity._ptr) throw new Error('specFromEntity needs a live Entity');
    const wPtr = alloc(this._m, weapon);
    try {
      const h = this._m._rpg_os_spec_from_entity(this._ptr, entity._ptr, wPtr.ptr);
      if (!h) throw new Error('failed to build a combatant from the entity');
      return this._trackSpec(h);
    } finally {
      free(this._m, wPtr.ptr);
    }
  }

  _trackSpec(h) {
    const spec = new CombatantSpec(this._m, h, () => this._specs.delete(h));
    this._specs.add(h);
    return spec;
  }

  /**
   * Runs one fight between two combatant specs. `options.maxRounds` (default
   * 1000) guards draws; `options.seed` reproduces a fight exactly;
   * `options.useMagic` (default true) lets spellcasters cast. Returns
   * `{ winner_index, rounds, max_lp, remaining_lp, a, b }` — run it in a loop
   * for Monte-Carlo win probabilities.
   */
  fight(specA, specB, options = {}) {
    if (!specA?._ptr || !specB?._ptr) throw new Error('fight needs two combatant specs');
    return jsonResult(
      this._m, this._m._rpg_os_fight, this._ptr, specA._ptr, specB._ptr,
      options.maxRounds ?? 1000, (options.seed ?? 0) >>> 0,
      (options.useMagic ?? true) ? 1 : 0);
  }

  /**
   * Runs one fight and returns the outcome plus its full transcript — the
   * individual dice rolls and stat changes of every round, from the opening
   * initiative roll to the final hit that decided the winner. Same options as
   * {@link fight}; the result additionally carries `hp_pool` and a `log`
   * object. The transcript is observation-only: the same seed produces exactly
   * the same fight as {@link fight}.
   */
  fightDetail(specA, specB, options = {}) {
    if (!specA?._ptr || !specB?._ptr) throw new Error('fightDetail needs two combatant specs');
    return jsonResult(
      this._m, this._m._rpg_os_fight_detail, this._ptr, specA._ptr, specB._ptr,
      options.maxRounds ?? 1000, (options.seed ?? 0) >>> 0,
      (options.useMagic ?? true) ? 1 : 0);
  }

  /** Releases the engine and every entity/spec it created. */
  dispose() {
    for (const h of this._entities) this._m._rpg_os_entity_free(h);
    for (const h of this._specs) this._m._rpg_os_spec_free(h);
    this._entities.clear();
    this._specs.clear();
    if (this._ptr) {
      this._m._rpg_os_engine_free(this._ptr);
      this._ptr = null;
    }
  }
}
