// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * Browser-side wrapper over the WASM flat C API. It mirrors the npm package's
 * API (wasm/package/index.mjs) without Node's `fs`, so a plain static page can
 * use the engine. The WASM module and every ruleset JSON are fetched by the
 * browser at runtime — nothing is compiled into the binary.
 */
import createRpgOsModule from './wasm/rpg-os-universal.js';

let modulePromise = null;

function getModule() {
  if (!modulePromise) {
    modulePromise = createRpgOsModule({
      locateFile: (file) => new URL(`./wasm/${file}`, import.meta.url).href,
    });
  }
  return modulePromise;
}

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

/** A live character sheet (wraps a C++ DynamicEntity handle). */
export class Entity {
  constructor(m, ptr, onDispose) {
    this._m = m;
    this._ptr = ptr;
    this._onDispose = onDispose;
  }

  setStat(stat, value) {
    const { ptr } = alloc(this._m, stat);
    try {
      this._m._rpg_os_entity_set_stat(this._ptr, ptr, value);
    } finally {
      free(this._m, ptr);
    }
    return this;
  }

  getStat(stat) {
    const { ptr } = alloc(this._m, stat);
    try {
      return this._m._rpg_os_entity_get_stat(this._ptr, ptr);
    } finally {
      free(this._m, ptr);
    }
  }

  getResource(pool) {
    const { ptr } = alloc(this._m, pool);
    try {
      return this._m._rpg_os_entity_get_resource(this._ptr, ptr);
    } finally {
      free(this._m, ptr);
    }
  }

  dispose() {
    if (this._ptr) {
      this._m._rpg_os_entity_free(this._ptr);
      this._ptr = null;
      this._onDispose?.();
    }
  }
}

/** A combatant description (wraps a C++ CombatantSpec handle). */
export class CombatantSpec {
  constructor(m, ptr, onDispose) {
    this._m = m;
    this._ptr = ptr;
    this._onDispose = onDispose;
  }

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

  meta() {
    return jsonResult(this._m, this._m._rpg_os_engine_meta, this._ptr);
  }

  entries() {
    return jsonResult(this._m, this._m._rpg_os_engine_entries, this._ptr);
  }

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

  createEntityFromSheet(id, stats) {
    const sheet = JSON.stringify({ id, stats: stats ?? {} });
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

  fight(specA, specB, options = {}) {
    if (!specA?._ptr || !specB?._ptr) throw new Error('fight needs two combatant specs');
    return jsonResult(
      this._m, this._m._rpg_os_fight, this._ptr, specA._ptr, specB._ptr,
      options.maxRounds ?? 1000, (options.seed ?? 0) >>> 0,
      (options.useMagic ?? true) ? 1 : 0);
  }

  /**
   * Runs one fight and returns the outcome plus its full transcript — the
   * individual dice rolls and stat changes of every round (same options and
   * seed semantics as {@link fight}; the result additionally carries
   * `hp_pool` and a `log` object).
   */
  fightDetail(specA, specB, options = {}) {
    if (!specA?._ptr || !specB?._ptr) throw new Error('fightDetail needs two combatant specs');
    return jsonResult(
      this._m, this._m._rpg_os_fight_detail, this._ptr, specA._ptr, specB._ptr,
      options.maxRounds ?? 1000, (options.seed ?? 0) >>> 0,
      (options.useMagic ?? true) ? 1 : 0);
  }

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

/** Initializes the engine (loads + instantiates the WASM module once). */
export async function init() {
  const m = await getModule();
  const enginePtr = m._rpg_os_engine_new();
  if (!enginePtr) throw new Error('failed to allocate the RPG OS engine');
  return new RpgOs(m, enginePtr);
}
