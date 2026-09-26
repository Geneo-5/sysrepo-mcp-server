# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

"""Merged schema introspection contract for get_schema."""

import pytest


def schema(client, **args):
    return client.tool("get_schema", args)


def test_schema_without_xpath_enumerates_implemented_modules(mcp_oven):
    result = schema(mcp_oven)

    assert "oven" in result["modules"]
    assert result["count"] == len(result["modules"])
    assert "nodes" in result["modules"]["oven"]


def test_schema_nodes_include_inline_paths_and_compiled_details(mcp_oven):
    result = schema(mcp_oven, xpath="/oven:oven")
    oven = result["modules"]["oven"]["nodes"]["oven"]
    temperature = oven["children"]["temperature"]

    assert oven["xpath"] == "/oven:oven"
    assert oven["type"] == "container"
    assert oven["presence"] is False
    assert temperature["xpath"] == "/oven:oven/temperature"
    assert temperature["base_type"] == "uint8"
    assert temperature["range"] == ["0..250"]
    assert temperature["default"] == "0"
    assert temperature["description"].strip()
    assert "nodes" not in result  # no redundant flat array


def test_schema_max_depth_zero_is_unlimited(mcp_module):
    result = schema(mcp_module, xpath="/sysrepo-mcp-test:server-state", max_depth=0)
    session = result["modules"]["sysrepo-mcp-test"]["nodes"]["server-state"]
    session = session["children"]["session"]

    assert "children" in session


def test_schema_max_depth_one_stops_at_direct_children(mcp_module):
    result = schema(mcp_module, xpath="/sysrepo-mcp-test:server-state", max_depth=1)
    session = result["modules"]["sysrepo-mcp-test"]["nodes"]["server-state"]
    session = session["children"]["session"]

    assert "children" not in session


def test_schema_max_depth_two_includes_grandchildren(mcp_module):
    result = schema(mcp_module, xpath="/sysrepo-mcp-test:server-state", max_depth=2)
    session = result["modules"]["sysrepo-mcp-test"]["nodes"]["server-state"]
    session = session["children"]["session"]

    assert "children" in session
    assert "session-id" in session["children"]


def test_schema_reports_list_keys_cardinality_and_assertions(mcp_module):
    result = schema(mcp_module, xpath="/sysrepo-mcp-test:api-key")
    api_key = result["modules"]["sysrepo-mcp-test"]["nodes"]["api-key"]

    assert api_key["keys"] == ["key"]
    assert api_key["min-elements"] == 0
    assert api_key["max-elements"] == "unbounded"
    assert "string-length" in api_key["must"][0]["expression"]
    assert "true()" in api_key["when"]
    assert api_key["children"]["user"]["mandatory"] is True


def test_schema_reports_rpc_input_with_xpath_usable_by_the_rpc_tool(mcp_oven):
    result = schema(mcp_oven, xpath="/oven:insert-food")
    rpc = result["modules"]["oven"]["nodes"]["insert-food"]
    time = rpc["children"]["input"]["children"]["time"]

    assert time["xpath"] == "/oven:insert-food/time"
    assert time["base_type"] == "enumeration"
    assert set(time["values"]) == {"now", "on-oven-ready"}


def test_schema_slash_means_all_implemented_modules(mcp_oven):
    omitted = schema(mcp_oven)
    explicit = schema(mcp_oven, xpath="/")

    assert explicit["modules"].keys() == omitted["modules"].keys()


def test_schema_unknown_xpath_is_not_found(mcp_oven):
    error = mcp_oven.tool_error("get_schema", {"xpath": "/oven:no-such-node"})

    assert error["code"] == -32001


def test_schema_rejects_negative_depth(mcp_oven):
    error = mcp_oven.tool_error("get_schema", {"max_depth": -1})

    assert error["code"] == -32602
