# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

"""
End-to-end tests against the upstream sysrepo oven plugin.

Everything here goes through lighttpd over HTTP, then FastCGI, then sysrepo,
then the oven plugin: the whole chain a real agent uses. The plugin is the one
shipped with sysrepo in extern/sysrepo/examples/plugin/oven.c, run by the
oven_plugin fixture, so these tests exercise real subscribers rather than a
stand-in.

The oven model, for reference:

    container oven                  config
        leaf turned-on   boolean            default false
        leaf temperature oven-temperature   default 0, uint8 range 0..250
    container oven-state            config false
        leaf temperature oven-temperature
        leaf food-inside boolean
    rpc insert-food                 input: leaf time, enum now|on-oven-ready
    rpc remove-food                 no input
    notification oven-ready
"""

import pytest

from .conftest import wait_for

OVEN = "/oven:oven"
OVEN_STATE = "/oven:oven-state"


def oven_config(client, xpath=OVEN):
    return client.get_config(xpath)["data"].get("oven:oven", {})


def oven_state(client):
    return client.get_operational(OVEN_STATE)["data"].get("oven:oven-state", {})


# ---------------------------------------------------------------------------
# Configuration: read
# ---------------------------------------------------------------------------


def test_reading_the_oven_container_succeeds(oven_off):
    result = oven_off.get_config(OVEN)

    assert isinstance(result["data"], dict)
    assert result["xpath"] == OVEN
    assert result["datastore"] == "running"


def test_reading_reports_the_datastore_it_read(oven_off):
    assert oven_off.get_config(OVEN, datastore="running")["datastore"] == "running"
    assert oven_off.get_config(OVEN, datastore="startup")["datastore"] == "startup"


def test_reading_an_xpath_that_matches_nothing_is_not_an_error(oven_off):
    # An empty match is a legitimate answer, not a failure. A server that
    # errors here forces an agent to guess whether a node is absent or the
    # request was wrong.
    result = oven_off.get_config("/oven:oven-state")

    assert result["data"] == {}


# ---------------------------------------------------------------------------
# Configuration: write
# ---------------------------------------------------------------------------


def test_setting_temperature_and_switch(oven_off):
    oven_off.edit_config({"oven:oven": {"turned-on": True, "temperature": 200}})

    config = oven_config(oven_off)

    assert config["turned-on"] is True
    assert config["temperature"] == 200


def test_merge_leaves_untouched_nodes_alone(oven_off):
    oven_off.edit_config({"oven:oven": {"turned-on": True, "temperature": 200}})
    oven_off.edit_config({"oven:oven": {"temperature": 150}})

    config = oven_config(oven_off)

    assert config["temperature"] == 150
    assert config["turned-on"] is True, "merge must not reset turned-on"


def test_explicit_merge_operation_behaves_like_the_default(oven_off):
    oven_off.edit_config({"oven:oven": {"turned-on": True, "temperature": 120}})
    oven_off.edit_config({"oven:oven": {"temperature": 130}}, operation="merge")

    config = oven_config(oven_off)

    assert config == {"turned-on": True, "temperature": 130}


def test_replace_operation_drops_unmentioned_nodes(oven_off):
    oven_off.edit_config({"oven:oven": {"turned-on": True, "temperature": 210}})
    oven_off.edit_config({"oven:oven": {"temperature": 90}}, operation="replace")

    config = oven_config(oven_off)

    assert config["temperature"] == 90
    # turned-on is back to its default, so it is either absent from the output
    # or explicitly false, depending on how defaults are printed.
    assert config.get("turned-on", False) is False


def test_edit_reports_the_operation_it_applied(oven_off):
    result = oven_off.edit_config({"oven:oven": {"temperature": 100}})

    assert result["ok"] is True
    assert result["operation"] == "merge"


def test_boundary_temperatures_are_accepted(oven_off):
    for value in (0, 1, 249, 250):
        oven_off.edit_config({"oven:oven": {"temperature": value}})

        assert oven_config(oven_off)["temperature"] == value


def test_a_write_then_read_round_trip_is_stable(oven_off):
    for value in (10, 99, 180, 250, 0):
        oven_off.edit_config({"oven:oven": {"temperature": value}})

        assert oven_config(oven_off).get("temperature", 0) == value


# ---------------------------------------------------------------------------
# Configuration: rejected writes
#
# The YANG schema is the contract. A value outside it must be refused by the
# server, not stored and discovered broken later.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("value", [251, 300, 1000, -1])
def test_temperature_outside_the_range_is_refused(oven_off, value):
    error = oven_off.tool_error(
        "sr_edit_config", {"config": {"oven:oven": {"temperature": value}}}
    )

    assert error["code"] == -32005, f"temperature {value} was not refused properly"

    assert oven_config(oven_off).get("temperature", 0) != value


def test_wrong_type_for_a_boolean_is_refused(oven_off):
    error = oven_off.tool_error(
        "sr_edit_config", {"config": {"oven:oven": {"turned-on": "maybe"}}}
    )

    assert error["code"] == -32005


def test_unknown_node_is_refused(oven_off):
    error = oven_off.tool_error(
        "sr_edit_config", {"config": {"oven:oven": {"no-such-leaf": 1}}}
    )

    assert error["code"] == -32005


def test_unknown_module_is_refused(oven_off):
    error = oven_off.tool_error(
        "sr_edit_config", {"config": {"no-such-module:thing": {"a": 1}}}
    )

    assert error["code"] in (-32003, -32005)


def test_a_refused_edit_leaves_the_datastore_untouched(oven_off):
    oven_off.edit_config({"oven:oven": {"temperature": 123}})

    oven_off.tool_error(
        "sr_edit_config", {"config": {"oven:oven": {"temperature": 999}}}
    )

    assert oven_config(oven_off)["temperature"] == 123


# ---------------------------------------------------------------------------
# Operational state, provided by the plugin
# ---------------------------------------------------------------------------


def test_oven_state_is_provided(oven_off):
    state = oven_state(oven_off)

    assert state, "the oven plugin provided no operational data"
    assert "food-inside" in state


def test_oven_state_temperature_is_within_the_yang_range(oven_off):
    state = oven_state(oven_off)

    if "temperature" not in state:
        pytest.skip("the plugin reported no oven-state/temperature")

    assert isinstance(state["temperature"], int)
    assert 0 <= state["temperature"] <= 250


def test_oven_state_food_inside_is_a_boolean(oven_off):
    assert isinstance(oven_state(oven_off)["food-inside"], bool)


def test_operational_read_of_a_single_leaf(oven_off):
    result = oven_off.get_operational(OVEN_STATE + "/food-inside")
    state = result["data"].get("oven:oven-state", {})

    assert "food-inside" in state


def test_operational_read_accepts_a_timeout(oven_off):
    state = oven_off.get_operational(OVEN_STATE, timeout_ms=10000)["data"]

    assert "oven:oven-state" in state


def test_operational_datastore_also_shows_configuration(oven_off):
    # The operational datastore is the merge of configuration and state, so a
    # value written to running must be visible here too.
    oven_off.edit_config({"oven:oven": {"temperature": 175}})

    data = oven_off.get_operational(OVEN)["data"]

    assert data.get("oven:oven", {}).get("temperature") == 175


def test_operational_tool_ignores_a_datastore_argument(oven_off):
    # sr_get_operational always reads the operational datastore. An extra
    # datastore argument is accepted and ignored rather than rejected, so an
    # agent copying arguments between tools is not blocked by it.
    result = oven_off.get_operational(OVEN_STATE, datastore="running")

    assert "oven:oven-state" in result["data"]


# ---------------------------------------------------------------------------
# RPCs, handled by the plugin
# ---------------------------------------------------------------------------


def test_insert_food_now_puts_food_inside(oven_off):
    assert oven_state(oven_off)["food-inside"] is False

    result = oven_off.tool(
        "sr_execute_rpc", {"xpath": "/oven:insert-food", "input": {"time": "now"}}
    )

    assert "output" in result

    state = wait_for(lambda: oven_state(oven_off).get("food-inside") or None)

    assert state is True, "the plugin did not report the food as inserted"


def test_remove_food_takes_it_out(oven_off):
    oven_off.tool(
        "sr_execute_rpc", {"xpath": "/oven:insert-food", "input": {"time": "now"}}
    )
    wait_for(lambda: oven_state(oven_off).get("food-inside") or None)

    oven_off.tool("sr_execute_rpc", {"xpath": "/oven:remove-food"})

    empty = wait_for(
        lambda: oven_state(oven_off).get("food-inside") is False or None
    )

    assert empty, "the plugin did not report the food as removed"


def test_rpc_without_input_needs_no_input_argument(oven_off):
    oven_off.tool(
        "sr_execute_rpc", {"xpath": "/oven:insert-food", "input": {"time": "now"}}
    )

    # remove-food declares no input at all.
    result = oven_off.tool("sr_execute_rpc", {"xpath": "/oven:remove-food"})

    assert result["output"] == {}


def test_insert_food_on_oven_ready_is_accepted(oven_off):
    oven_off.edit_config({"oven:oven": {"turned-on": True, "temperature": 250}})

    # The oven starts cold, so the plugin defers the insertion until the
    # oven-ready notification. The call itself must still succeed.
    result = oven_off.tool(
        "sr_execute_rpc",
        {"xpath": "/oven:insert-food", "input": {"time": "on-oven-ready"}},
    )

    assert "output" in result


def test_invalid_enum_value_is_refused(oven_off):
    error = oven_off.tool_error(
        "sr_execute_rpc",
        {"xpath": "/oven:insert-food", "input": {"time": "whenever"}},
    )

    assert error["code"] == -32005


def test_unknown_rpc_is_not_found(oven_off):
    error = oven_off.tool_error("sr_execute_rpc", {"xpath": "/oven:no-such-rpc"})

    assert error["code"] == -32003


def test_rpc_xpath_is_required(oven_off):
    error = oven_off.tool_error("sr_execute_rpc", {"input": {"time": "now"}})

    assert error["code"] == -32602


def test_sr_action_shares_the_rpc_path(oven_off):
    # oven.yang defines no action, so this checks the tool is wired at all and
    # reports a missing operation the same way sr_execute_rpc does.
    error = oven_off.tool_error("sr_action", {"xpath": "/oven:no-such-action"})

    assert error["code"] == -32003


# ---------------------------------------------------------------------------
# The whole scenario, in order
# ---------------------------------------------------------------------------


def test_full_oven_session(oven_off):
    """
    Drive the oven the way an agent would: configure it, verify, cook,
    verify, clean up.
    """
    client = oven_off

    # 1. The oven starts off and empty.
    assert oven_state(client)["food-inside"] is False

    # 2. Turn it on and set a target temperature.
    client.edit_config({"oven:oven": {"turned-on": True, "temperature": 220}})

    config = oven_config(client)
    assert config["turned-on"] is True
    assert config["temperature"] == 220

    # 3. Put the food in.
    client.tool(
        "sr_execute_rpc", {"xpath": "/oven:insert-food", "input": {"time": "now"}}
    )

    assert wait_for(lambda: oven_state(client).get("food-inside") or None) is True

    # 4. The operational view now shows both the configuration and the state.
    operational = client.get_operational("/oven:*")["data"]
    assert "oven:oven-state" in operational or "oven:oven" in operational

    # 5. Take the food out and switch off.
    client.tool("sr_execute_rpc", {"xpath": "/oven:remove-food"})

    assert wait_for(
        lambda: oven_state(client).get("food-inside") is False or None
    )

    client.edit_config({"oven:oven": {"turned-on": False}})

    assert oven_config(client)["turned-on"] is False
