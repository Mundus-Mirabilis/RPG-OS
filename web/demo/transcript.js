// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * Pure DOM builders for the single-fight transcript. They only create
 * elements from their arguments (no shared state), so they are reused by the
 * duel panel in app.js and stay trivially testable.
 */

/** A monospaced `.dice` badge showing one dice result (e.g. "6 6"). */
export function diceBadge(text) {
  const span = document.createElement('span');
  span.className = 'dice';
  span.textContent = text;
  return span;
}

/** " · target H → H' pool" — the hit-point change one action caused. */
export function hpChange(target, before, after, pool) {
  const span = document.createElement('span');
  span.className = 'fight-hp';
  span.textContent = before === after
    ? ` · ${target} at ${after} ${pool}`
    : ` · ${target} ${before} → ${after} ${pool}`;
  return span;
}

/** "name — spells: X, Y" line per spellcasting combatant, or null when
 *  neither knows a spell. Shown above the rounds so the reader can see who is
 *  a spellcaster and what it can cast before the dice start rolling. */
export function renderKnownSpells(aName, aSpells, bName, bSpells) {
  const wrap = document.createElement('div');
  wrap.className = 'fight-spells';
  const add = (name, spells) => {
    if (!spells || spells.length === 0) return;
    const line = document.createElement('div');
    const who = document.createElement('b');
    who.textContent = name;
    line.append(who, ' — spells: ', spells.join(', '));
    wrap.append(line);
  };
  add(aName, aSpells);
  add(bName, bSpells);
  return wrap.childNodes.length > 0 ? wrap : null;
}

/** The icon that opens an action line: ⚔️ a weapon attack, ✨ a spell cast. */
const ACTION_ICONS = { attack: '⚔️', cast: '✨' };

/** "✅ hit" / "❌ miss" — the outcome marker after the arrow. */
function outcomeBadge(ok, word) {
  const span = document.createElement('span');
  span.className = ok ? 'fight-ok' : 'fight-bad';
  span.textContent = `${ok ? '✅' : '❌'} ${word}`;
  return span;
}

/** " · AE 35 → 27" — how the caster's spell-resource pool was spent, or "". */
function resourceChange(action) {
  return action.resource && action.resource_before !== action.resource_after
    ? ` · ${action.resource} ${action.resource_before} → ${action.resource_after}`
    : '';
}

/** One action line of a round's transcript (an attack or a spell cast). */
export function renderFightAction(action, aName, bName, hpPool) {
  const actor = action.actor === 0 ? aName : bName;
  const target = action.target === 0 ? aName : bName;
  const row = document.createElement('div');
  row.className = 'fight-action';

  const icon = document.createElement('span');
  icon.className = 'fight-icon';
  icon.textContent = ACTION_ICONS[action.kind] ?? '•';
  row.append(icon);

  const actorEl = document.createElement('b');
  actorEl.textContent = actor;
  row.append(actorEl);

  if (action.kind === 'cast') {
    const spellName = action.spell_name || action.spell;
    const res = resourceChange(action);
    row.append(' casts ');
    const spellEl = document.createElement('b');
    spellEl.textContent = spellName;
    row.append(spellEl);
    // A casting check only exists when the ruleset declares one (TDE's 3d20
    // pool); D&D-style spells resolve without a check, so no dice and no
    // success arrow are shown there.
    if (action.check_dice.length > 0) {
      row.append(' — check ');
      row.append(diceBadge(action.check_dice.join(' ')));
      row.append(' ');
      row.append(outcomeBadge(action.is_hit, action.is_hit ? 'success' : 'failed'));
    }
    if (action.is_hit) {
      if (action.damage > 0) {
        row.append(' · ');
        // Some effects deal flat (0-damage-expression) damage — no dice.
        if (action.damage_dice.length > 0) {
          row.append(diceBadge(action.damage_dice.join(' ')));
          row.append(' ');
        }
        row.append('deals ');
        const dmgEl = document.createElement('b');
        dmgEl.textContent = `${action.damage} damage`;
        row.append(dmgEl);
      }
      // The resource is spent on the attempt too, so a fizzle still costs.
      if (res) row.append(res);
    } else {
      // A failed casting check (dice shown above) still spends the resource.
      if (res) row.append(res);
    }
    row.append(hpChange(target, action.hp_before, action.target_hp, hpPool));
  } else {
    row.append(' attacks ');
    const targetEl = document.createElement('b');
    targetEl.textContent = target;
    row.append(targetEl);
    row.append(' — d20 ');
    row.append(diceBadge(action.check_dice.join(' ')));
    row.append(' ');
    row.append(outcomeBadge(action.is_hit, action.is_hit ? 'hit' : 'miss'));
    if (!action.is_hit) {
      row.append(hpChange(target, action.hp_before, action.target_hp, hpPool));
    } else {
      row.append(' · damage ');
      // Some bestiary attacks have a flat (0-damage) expression — no dice.
      if (action.damage_dice.length > 0) {
        row.append(diceBadge(action.damage_dice.join(' ')));
        row.append(' ');
      }
      const dmgEl = document.createElement('b');
      dmgEl.textContent = `= ${action.damage}`;
      row.append(dmgEl);
      row.append(hpChange(target, action.hp_before, action.target_hp, hpPool));
    }
  }
  return row;
}

/** One round of a fight's transcript: the initiative roll and its actions. */
export function renderFightRound(round, aName, bName, hpPool) {
  const wrap = document.createElement('div');
  wrap.className = 'fight-round';

  const head = document.createElement('div');
  head.className = 'fight-round-head';
  const firstName = round.goes_first === 0 ? aName : bName;
  head.append(`Round ${round.round} — initiative `);
  head.append(diceBadge(`${aName} d6 ${round.init_roll[0]} → ${round.init_total[0]}`));
  head.append(document.createTextNode(' · '));
  head.append(diceBadge(`${bName} d6 ${round.init_roll[1]} → ${round.init_total[1]}`));
  head.append(` — ${firstName} acts first`);
  wrap.append(head);

  for (const action of round.actions) {
    wrap.append(renderFightAction(action, aName, bName, hpPool));
  }
  return wrap;
}
