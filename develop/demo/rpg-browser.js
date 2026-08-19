// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * Browser wrapper over the shared WASM wrapper (rpg-core.js). It imports the
 * demo's local WASM glue (./wasm/), provides init(), and re-exports the shared
 * Entity/CombatantSpec/RpgOs classes — the same engine API as the npm package,
 * without Node's `fs`. The WASM module and every ruleset JSON are fetched by
 * the browser at runtime; nothing is compiled into the binary.
 */
import createRpgOsModule from './wasm/rpg-os-universal.js';
import { Entity, CombatantSpec, RpgOs } from './rpg-core.js';

let modulePromise = null;

function getModule() {
  if (!modulePromise) {
    modulePromise = createRpgOsModule({
      locateFile: (file) => new URL(`./wasm/${file}`, import.meta.url).href,
    });
  }
  return modulePromise;
}

/** Initializes the engine (loads + instantiates the WASM module once). */
export async function init() {
  const m = await getModule();
  const enginePtr = m._rpg_os_engine_new();
  if (!enginePtr) throw new Error('failed to allocate the RPG-OS engine');
  return new RpgOs(m, enginePtr);
}

export { Entity, CombatantSpec, RpgOs };
export default init;
