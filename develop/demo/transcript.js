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

/** One action line of a round's transcript (an attack or a spell cast). */
export function renderFightAction(action, aName, bName, hpPool) {
  const actor = action.actor === 0 ? aName : bName;
  const target = action.target === 0 ? aName : bName;
  const row = document.createElement('div');
  row.className = 'fight-action';

  const actorEl = document.createElement('b');
  actorEl.textContent = actor;
  row.append(actorEl);

  if (action.kind === 'cast') {
    row.append(' casts ');
    const spellEl = document.createElement('b');
    spellEl.textContent = action.spell;
    row.append(spellEl);
    row.append(' — check ');
    row.append(diceBadge(action.check_dice.join(' ')));
    row.append(action.is_hit ? ' → success' : ' → failed');
    if (action.is_hit && action.damage > 0) {
      row.append(' · deals ');
      const dmgEl = document.createElement('b');
      dmgEl.textContent = `${action.damage} damage`;
      row.append(dmgEl);
      if (action.cost > 0) {
        row.append(` (cost ${action.cost} ${action.resource})`);
      }
    }
    row.append(hpChange(target, action.hp_before, action.target_hp, hpPool));
  } else {
    row.append(' attacks ');
    const targetEl = document.createElement('b');
    targetEl.textContent = target;
    row.append(targetEl);
    row.append(' — d20 ');
    row.append(diceBadge(action.check_dice.join(' ')));
    if (!action.is_hit) {
      row.append(' → miss');
      row.append(hpChange(target, action.hp_before, action.target_hp, hpPool));
    } else {
      row.append(' → hit · damage ');
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
