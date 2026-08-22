# Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
# SPDX-License-Identifier: Apache-2.0

# Test suite for rpg_os — doctest (vendored single header).
#
# The library itself is header-only; only this test executable is compiled.
# doctest is included as <doctest/doctest.h> from include/rpg_os/third_party,
# which is exposed via the rpg_os target's SYSTEM include directory.
#
# Add each new test translation unit here as it is created (TDD: the test
# file is written before the code it exercises).

add_executable(rpg_os_tests
  test_main.cpp
  test_smoke.cpp
  test_event_system.cpp
  test_dice.cpp
  test_math.cpp
  test_cost_table.cpp
  test_modifier.cpp
  test_checks.cpp
  test_entity.cpp
  test_entity_registry.cpp
  test_expression.cpp
  test_ruleset_loader.cpp
  test_engine.cpp
  test_rulesets.cpp
  test_variance.cpp
  test_combat.cpp
  test_combat_session.cpp
  test_codegen_output.cpp
  test_data_api.cpp
  test_money.cpp
  test_inventory.cpp
  test_equipment.cpp
  test_effects.cpp
  test_spellcasting.cpp
  test_conditions.cpp
  test_traits.cpp
  test_advancement.cpp
  test_game_session.cpp
  test_check_resolver.cpp
  test_world_state.cpp
  test_movement.cpp
  test_types.cpp
  test_errors.cpp
)

target_link_libraries(rpg_os_tests PRIVATE rpg_os::rpg_os)
target_compile_features(rpg_os_tests PRIVATE cxx_std_23)
# Tests that load the shipped ruleset files need to locate them on disk.
target_compile_definitions(rpg_os_tests PRIVATE RPG_OS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

set_project_warnings(rpg_os_tests)
enable_sanitizers(rpg_os_tests)

add_test(NAME rpg_os_tests COMMAND rpg_os_tests)
