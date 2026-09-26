# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

"""
MCP lifecycle, tool catalogue and result envelope.

An MCP client begins every connection with initialize and then reads
tools/list; a server that answers neither is unusable by an unmodified agent
whatever its tools do. These tests check the handshake, the catalogue and the
shape of a tool result.
"""

import json

import pytest

from .conftest import MCP_PROTOCOL_VERSION

# Every tool the server is expected to advertise.
EXPECTED_TOOLS = {
    "get_status",
    "get_schema",
    "sr_get_config",
    "sr_edit_config",
    "sr_delete_config",
    "sr_copy_config",
    "sr_get_operational",
    "sr_execute_rpc",
    "sr_action",
    "sr_notif_subscribe",
    "sr_notif_unsubscribe",
    "sr_notif_list_subscriptions",
    "sr_notif_poll",
    "sr_notif_send",
    "sr_list_modules",
    "sr_module_install",
    "sr_module_uninstall",
}


# ---------------------------------------------------------------------------
# initialize
# ---------------------------------------------------------------------------


def test_initialize_returns_a_protocol_version(mcp):
    result = mcp.result(
        "initialize",
        {
            "protocolVersion": MCP_PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "sysrepo-mcp-tests", "version": "1.0.0"},
        },
    )

    assert result["protocolVersion"] == MCP_PROTOCOL_VERSION


def test_initialize_announces_the_tool_capability(mcp):
    result = mcp.result("initialize", {"protocolVersion": MCP_PROTOCOL_VERSION})

    assert "tools" in result["capabilities"]

    # No resources, no prompts, no server-initiated messages: this transport
    # cannot carry them, and announcing them would make a client wait.
    assert "resources" not in result["capabilities"]
    assert "prompts" not in result["capabilities"]
    assert "sampling" not in result["capabilities"]


def test_initialize_identifies_the_server(mcp):
    result = mcp.result("initialize", {"protocolVersion": MCP_PROTOCOL_VERSION})
    info = result["serverInfo"]

    assert info["name"] == "sysrepo-mcp"
    assert info["version"]


def test_initialized_notification_is_accepted(mcp):
    response = mcp.rpc("notifications/initialized", req_id=None)

    assert response.status == 202


def test_full_handshake(mcp):
    mcp.result("initialize", {"protocolVersion": MCP_PROTOCOL_VERSION})
    assert mcp.rpc("notifications/initialized", req_id=None).status == 202

    # After the handshake the catalogue must be reachable.
    assert mcp.result("tools/list")["tools"]


# ---------------------------------------------------------------------------
# tools/list
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def catalogue(mcp):
    return {tool["name"]: tool for tool in mcp.result("tools/list")["tools"]}


def test_catalogue_holds_every_expected_tool(catalogue):
    assert EXPECTED_TOOLS <= set(catalogue)


def test_catalogue_holds_no_unexpected_tool(catalogue):
    # A tool that is advertised but unimplemented is worse than a missing one:
    # the agent calls it and gets a failure it cannot interpret.
    assert set(catalogue) <= EXPECTED_TOOLS


@pytest.mark.parametrize("name", sorted(EXPECTED_TOOLS))
def test_every_tool_is_described(catalogue, name):
    tool = catalogue[name]

    assert tool["description"].strip(), f"{name} has no description"
    assert tool["description"].endswith("."), f"{name} description is not a sentence"


@pytest.mark.parametrize("name", sorted(EXPECTED_TOOLS))
def test_every_tool_has_a_usable_input_schema(catalogue, name):
    schema = catalogue[name]["inputSchema"]

    assert schema["type"] == "object", f"{name} schema is not an object schema"
    assert isinstance(schema.get("properties"), dict), f"{name} declares no properties"

    # Whatever a schema marks required must be one of its own properties,
    # otherwise an agent cannot satisfy it.
    for required in schema.get("required", []):
        assert required in schema["properties"], (
            f"{name} requires {required}, which it does not declare"
        )


def test_xpath_tools_require_an_xpath(catalogue):
    for name in ("sr_get_config", "sr_delete_config", "sr_get_operational",
                 "sr_execute_rpc", "sr_action", "sr_notif_send"):
        assert "xpath" in catalogue[name]["inputSchema"].get("required", []), (
            f"{name} should require xpath"
        )


def test_schema_xpath_is_optional(catalogue):
    schema = catalogue["get_schema"]["inputSchema"]

    assert "xpath" in schema["properties"]
    assert "xpath" not in schema.get("required", [])


def test_stateful_tools_are_documented_as_such(catalogue):
    # An agent reads the description to decide whether it needs a session.
    for name in ("sr_notif_subscribe", "sr_notif_poll",
                 "sr_notif_list_subscriptions"):
        assert "session" in catalogue[name]["description"].lower(), (
            f"{name} keeps state per session and should say so"
        )


def test_datastore_enums_agree_with_the_documentation(catalogue):
    for name in ("sr_get_config", "sr_edit_config", "sr_delete_config"):
        enum = catalogue[name]["inputSchema"]["properties"]["datastore"]["enum"]

        assert set(enum) == {"running", "startup", "candidate"}, (
            f"{name} advertises datastores {enum}"
        )


def test_operational_tool_takes_no_datastore(catalogue):
    # sr_get_operational always reads the operational datastore; accepting a
    # datastore argument would suggest otherwise.
    properties = catalogue["sr_get_operational"]["inputSchema"]["properties"]

    assert "datastore" not in properties


# ---------------------------------------------------------------------------
# tools/call envelope
# ---------------------------------------------------------------------------


def test_tool_result_uses_the_mcp_content_envelope(mcp):
    response = mcp.call("get_status", {})
    result = response.json()["result"]

    assert isinstance(result["content"], list)
    assert result["content"][0]["type"] == "text"
    assert result["isError"] is False


def test_tool_text_block_matches_structured_content(mcp):
    result = mcp.call("get_status", {}).json()["result"]

    assert json.loads(result["content"][0]["text"]) == result["structuredContent"]


def test_tool_arguments_are_optional(mcp):
    # get_status takes none; omitting arguments entirely must work.
    response = mcp.rpc("tools/call", {"name": "get_status"})

    assert response.status == 200
    assert "error" not in response.json()


def test_unknown_tool_is_method_not_found(mcp):
    error = mcp.tool_error("no_such_tool", {})

    assert error["code"] == -32601


def test_tools_call_without_a_name_is_invalid_params(mcp):
    error = mcp.error("tools/call", {"arguments": {}})

    assert error["code"] == -32602


def test_tools_call_with_non_object_arguments_is_invalid_params(mcp):
    error = mcp.error("tools/call", {"name": "get_status", "arguments": [1, 2]})

    assert error["code"] == -32602


# ---------------------------------------------------------------------------
# get_status
# ---------------------------------------------------------------------------


def test_get_status_reports_the_expected_fields(mcp):
    status = mcp.tool("get_status", {})

    assert isinstance(status["version"], str) and status["version"]
    assert isinstance(status["uptime_seconds"], int)
    assert isinstance(status["active_sessions"], int)
    assert isinstance(status["max_sessions"], int)
    assert status["max_sessions"] >= 1
    assert status["session_ttl_seconds"] >= 60


def test_get_status_version_matches_the_server_info(mcp):
    initialize = mcp.result("initialize", {"protocolVersion": MCP_PROTOCOL_VERSION})
    status = mcp.tool("get_status", {})

    assert status["version"] == initialize["serverInfo"]["version"]


def test_get_status_uptime_is_not_negative(mcp):
    assert mcp.tool("get_status", {})["uptime_seconds"] >= 0


def test_get_status_verbose_lists_sessions(mcp):
    status = mcp.tool("get_status", {"verbose": True})

    assert isinstance(status["sessions"], list)


def test_get_status_without_verbose_omits_the_session_list(mcp):
    assert "sessions" not in mcp.tool("get_status", {"verbose": False})
