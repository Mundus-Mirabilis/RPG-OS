// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file main.cpp
 * @brief Specific mode demo: use the code-generated, strongly typed headers.
 *
 * Attributes are named members (@c geron.courage), derived stats are named
 * getters with compiled formulas (@c geron.maxLifePoints()), and checks are
 * named methods (@c geron.checkClimbing(...)) — while the character data still
 * comes from the ruleset JSON at runtime via
 * @c rpg_os::generated::tde5e::Character::fromArchetype.
 *
 * @par Why this mode?
 * It shows the trade the code generator makes: the *schema* is compiled in
 * (type-safe, allocation-free, inlineable), but the *data* is still loaded
 * from JSON so creatures and items never need a rebuild. This is the demo
 * counterpart to @c examples/universal_mode/main.cpp — both print the same
 * numbers for the same character.
 *
 * Usage: @c rpg_os_example_specific [project-root]
 */
#include <dnd5e_srd_static.hpp>
#include <fstream>
#include <iostream>
#include <rpg_os/common/json.hpp>
#include <rpg_os/core/dice_engine.hpp>
#include <sstream>
#include <string>
#include <tde5e_core_static.hpp>

namespace {

/// Joins the project root with a ruleset file name (see
/// @c examples/universal_mode/main.cpp for the same helper — it is kept
/// local to each example so the demos stay fully self-contained).
std::string rulesetPath(std::string_view root, std::string_view name) {
  return std::string(root) + "/rulesets/" + std::string(name);
}

/// Reads a ruleset file into a @c rpg_os::Json. The generated character
/// classes parse the *whole* ruleset document (schema + data) themselves;
/// this helper just fetches the bytes and hands them over.
rpg_os::Json loadRuleset(const std::string &root, std::string_view name) {
  std::ifstream file(rulesetPath(root, name));
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return rpg_os::Json::parse(buffer.str());
}

} // namespace

int main(int argc, char **argv) {
  // Optional leading positional argument = project root, matching the sibling
  // examples so all three demos share the same invocation convention.
  const std::string root = argc > 1 ? argv[1] : ".";

  std::cout << "=== The Dark Eye 5e (specific mode) ===\n";
  const rpg_os::Json tde = loadRuleset(root, "tde5e_core.json");
  const auto geron = rpg_os::generated::tde5e::Character::fromArchetype(tde, "geron");
  // Byte-sized members stream as characters; cast to int for display.
  std::cout << "Geron: COU " << static_cast<int>(geron.courage) << ", Life Points "
            << geron.lifePoints << '/' << geron.maxLifePoints() << ", Attack "
            << geron.attackSwordsSr6() << ", Dodge " << geron.dodge() << '\n';

  rpg_os::CheckParams params;
  rpg_os::DefaultRandom rng;
  // The generated check method instantiates the shared 3d20 pool algorithm
  // with the skill's attributes baked in — same result as universal mode.
  const rpg_os::CheckResult climb = geron.checkClimbing(params, rng);
  std::cout << "Climbing check: " << (climb.isSuccess ? "success" : "failure") << " (SP left "
            << climb.remainingPool << ", QL " << climb.qualityLevel << ")\n";

  std::cout << "=== D&D 5e SRD (specific mode) ===\n";
  const rpg_os::Json dnd = loadRuleset(root, "dnd5e_srd.json");
  // The SRD ruleset stores text descriptions (classes/species/backgrounds/feats)
  // rather than PC archetype blocks, so load a bestiary entry instead.
  const auto fighter = rpg_os::generated::dnd5e::Character::fromCreature(dnd, "goblin_warrior");
  std::cout << "Goblin: STR " << static_cast<int>(fighter.strength) << " (mod "
            << fighter.strengthModifier() << "), AC " << fighter.armorClass() << ", HP "
            << fighter.hitPoints << '\n';

  rpg_os::CheckParams attackParams;
  rpg_os::DefaultRandom attackRng;
  // A generated check method taking the target as a StatProvider argument.
  const rpg_os::CheckResult attack = fighter.attackMelee(fighter, attackParams, attackRng);
  std::cout << "Melee attack vs AC " << fighter.armorClass() << ": "
            << (attack.isSuccess ? "hit" : "miss") << " (roll "
            << (attack.rawDiceRolls.empty() ? 0 : attack.rawDiceRolls.front()) << ")\n";
  return 0;
}
