# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

"""
Schema introspection: get_tree and get_help.

These are how an agent discovers what it may address before touching any data,
so they are checked against the oven module, whose shape is known exactly.

They use mcp_oven rather than oven_plugin: the schema lives in the libyang
context, not in a subscriber, so no plugin needs to be running.
"""

import pytest

# Every schema node of oven.yang, with its node type.
OVEN_NODES = {
    "/oven:oven": "container",
    "/oven:oven/turned-on": "leaf",
    "/oven:oven/temperature": "leaf",
    "/oven:oven-state": "container",
    "/oven:oven-state/temperature": "leaf",
    "/oven:oven-state/food-inside": "leaf",
}

OVEN_OPERATIONS = {"/oven:insert-food", "/oven:remove-food"}


@pytest.fixture(scope="module")
def tree(mcp_oven):
    return mcp_oven.tool("get_tree", {"module": "oven"})


@pytest.fixture(scope="module")
def described_tree(mcp_oven):
    return mcp_oven.tool("get_tree", {"module": "oven", "with_descriptions": True})


# ---------------------------------------------------------------------------
# get_tree
# ---------------------------------------------------------------------------


def test_tree_identifies_the_module(tree):
    header = tree["tree"]

    assert header["module"] == "oven"
    assert header["namespace"] == "urn:sysrepo:oven"
    assert header["prefix"] == "ov"
    assert header["revision"] == "2018-01-19"


def test_tree_exposes_both_containers(tree):
    roots = tree["tree"]["nodes"]

    assert "oven" in roots
    assert "oven-state" in roots


def test_tree_marks_state_data_as_non_configurable(tree):
    roots = tree["tree"]["nodes"]

    assert roots["oven"]["config"] is True
    assert roots["oven-state"]["config"] is False


def test_tree_exposes_the_children_of_a_container(tree):
    children = tree["tree"]["nodes"]["oven"]["children"]

    assert set(children) == {"turned-on", "temperature"}
    assert children["temperature"]["type"] == "leaf"
    assert children["temperature"]["xpath"] == "/oven:oven/temperature"


def test_tree_exposes_the_rpcs(tree):
    roots = tree["tree"]["nodes"]

    assert "insert-food" in roots, "get_tree omits the module RPCs"
    assert "remove-food" in roots
    assert roots["insert-food"]["type"] == "rpc"


def test_flat_node_list_covers_every_data_node(tree):
    found = {node["xpath"]: node["type"] for node in tree["nodes"]}

    for xpath, node_type in OVEN_NODES.items():
        assert xpath in found, f"{xpath} missing from the flat node list"
        assert found[xpath] == node_type, f"{xpath} typed as {found[xpath]}"


def test_flat_node_list_covers_the_operations(tree):
    found = {node["xpath"] for node in tree["nodes"]}

    assert OVEN_OPERATIONS <= found


def test_flat_node_list_has_no_duplicates(tree):
    paths = [node["xpath"] for node in tree["nodes"]]

    assert len(paths) == len(set(paths))


def test_descriptions_are_omitted_by_default(tree):
    assert "description" not in tree["tree"]["nodes"]["oven"]


def test_descriptions_are_included_on_request(described_tree):
    oven = described_tree["tree"]["nodes"]["oven"]

    assert "description" in oven
    assert oven["description"].strip()


def test_subtree_request_narrows_the_result(mcp_oven):
    result = mcp_oven.tool("get_tree", {"module": "oven", "xpath": "/oven:oven"})
    roots = result["tree"]["nodes"]

    assert set(roots) == {"oven"}
    assert set(roots["oven"]["children"]) == {"turned-on", "temperature"}


def test_subtree_request_on_a_leaf(mcp_oven):
    result = mcp_oven.tool(
        "get_tree", {"module": "oven", "xpath": "/oven:oven/temperature"}
    )
    roots = result["tree"]["nodes"]

    assert set(roots) == {"temperature"}
    assert "children" not in roots["temperature"]


def test_schema_depth_zero_is_unlimited(mcp_module):
    result = mcp_module.tool(
        "get_tree", {"module": "sysrepo-mcp-test",
                     "xpath": "/sysrepo-mcp-test:server-state",
                     "max_depth": 0}
    )
    session = result["tree"]["nodes"]["server-state"]["children"]["session"]

    assert "children" in session


def test_schema_depth_one_stops_at_direct_children(mcp_module):
    result = mcp_module.tool(
        "get_tree", {"module": "sysrepo-mcp-test",
                     "xpath": "/sysrepo-mcp-test:server-state",
                     "max_depth": 1}
    )
    session = result["tree"]["nodes"]["server-state"]["children"]["session"]

    assert "children" not in session


def test_schema_depth_two_includes_grandchildren(mcp_module):
    result = mcp_module.tool(
        "get_tree", {"module": "sysrepo-mcp-test",
                     "xpath": "/sysrepo-mcp-test:server-state",
                     "max_depth": 2}
    )
    session = result["tree"]["nodes"]["server-state"]["children"]["session"]

    assert "children" in session


def test_tree_reports_list_constraints_and_keys(mcp_module):
    result = mcp_module.tool(
        "get_tree", {"module": "sysrepo-mcp-test",
                     "xpath": "/sysrepo-mcp-test:api-key"}
    )
    node = result["tree"]["nodes"]["api-key"]

    assert node["keys"] == ["key"]
    assert node["min-elements"] == 0
    assert node["max-elements"] == "unbounded"


def test_tree_reports_mandatory_and_presence(mcp_module):
    result = mcp_module.tool(
        "get_tree", {"module": "sysrepo-mcp-test",
                     "xpath": "/sysrepo-mcp-test:api-key"}
    )
    node = result["tree"]["nodes"]["api-key"]

    assert node["children"]["user"]["mandatory"] is True
    state = mcp_module.tool(
        "get_tree", {"module": "sysrepo-mcp-test",
                     "xpath": "/sysrepo-mcp-test:server-state"}
    )["tree"]["nodes"]["server-state"]
    assert state["presence"] is False


def test_module_root_and_explicit_slash_agree(mcp_oven, tree):
    explicit = mcp_oven.tool("get_tree", {"module": "oven", "xpath": "/"})

    assert explicit["tree"]["nodes"].keys() == tree["tree"]["nodes"].keys()


def test_tree_reports_imports(tree):
    # oven.yang imports nothing, so the list must be present and empty rather
    # than missing: an agent should not have to tell "no imports" from
    # "imports not reported".
    assert tree["imports"] == []


def test_unknown_module_is_not_found(mcp_oven):
    error = mcp_oven.tool_error("get_tree", {"module": "no-such-module"})

    assert error["code"] == -32001


def test_unknown_subtree_is_not_found(mcp_oven):
    error = mcp_oven.tool_error(
        "get_tree", {"module": "oven", "xpath": "/oven:no-such-node"}
    )

    assert error["code"] == -32001


def test_module_is_required(mcp_oven):
    error = mcp_oven.tool_error("get_tree", {})

    assert error["code"] == -32602


# ---------------------------------------------------------------------------
# get_help
# ---------------------------------------------------------------------------


def test_help_on_a_leaf(mcp_oven):
    help_ = mcp_oven.tool("get_help", {"xpath": "/oven:oven/temperature"})

    assert help_["xpath"] == "/oven:oven/temperature"
    assert help_["node_type"] == "leaf"
    assert help_["config"] is True
    assert help_["mandatory"] is False
    assert "temperature" in help_["description"].lower()


def test_help_on_a_container(mcp_oven):
    help_ = mcp_oven.tool("get_help", {"xpath": "/oven:oven"})

    assert help_["node_type"] == "container"
    assert help_["config"] is True


def test_help_on_a_state_node(mcp_oven):
    help_ = mcp_oven.tool("get_help", {"xpath": "/oven:oven-state/food-inside"})

    assert help_["node_type"] == "leaf"
    assert help_["config"] is False


def test_help_on_an_rpc(mcp_oven):
    help_ = mcp_oven.tool("get_help", {"xpath": "/oven:insert-food"})

    assert help_["node_type"] == "rpc"


def test_help_names_the_module_and_namespace(mcp_oven):
    help_ = mcp_oven.tool("get_help", {"xpath": "/oven:oven/turned-on"})

    assert help_["module"] == "oven"
    assert help_["namespace"] == "urn:sysrepo:oven"


@pytest.mark.parametrize("xpath", sorted(OVEN_NODES))
def test_help_answers_for_every_node_of_the_module(mcp_oven, xpath):
    help_ = mcp_oven.tool("get_help", {"xpath": xpath})

    assert help_["xpath"] == xpath
    assert help_["node_type"] == OVEN_NODES[xpath]
    assert help_["description"].strip(), f"{xpath} has no description"


def test_help_on_an_unknown_node_is_not_found(mcp_oven):
    error = mcp_oven.tool_error("get_help", {"xpath": "/oven:oven/no-such-leaf"})

    assert error["code"] == -32001


def test_help_on_an_unknown_module_is_not_found(mcp_oven):
    error = mcp_oven.tool_error("get_help", {"xpath": "/no-such-module:thing"})

    assert error["code"] == -32001


def test_help_xpath_is_required(mcp_oven):
    error = mcp_oven.tool_error("get_help", {})

    assert error["code"] == -32602


def test_help_reports_the_leaf_type(mcp_oven):
    help_ = mcp_oven.tool("get_help", {"xpath": "/oven:oven/temperature"})

    # The typedef name (oven-temperature) is not recoverable from the
    # compiled schema, so only the base type is reported.
    assert help_["base_type"] == "uint8"


def test_help_reports_a_boolean_leaf(mcp_oven):
    help_ = mcp_oven.tool("get_help", {"xpath": "/oven:oven/turned-on"})

    assert help_["base_type"] == "boolean"


def test_help_reports_enumeration_values(mcp_oven):
    help_ = mcp_oven.tool("get_help", {"xpath": "/oven:insert-food/time"})

    assert help_["base_type"] == "enumeration"
    assert set(help_["values"]) == {"now", "on-oven-ready"}


def test_help_omits_type_details_for_a_container(mcp_oven):
    help_ = mcp_oven.tool("get_help", {"xpath": "/oven:oven"})

    assert "base_type" not in help_


def test_help_reports_the_range_and_default(mcp_oven):
    help_ = mcp_oven.tool("get_help", {"xpath": "/oven:oven/temperature"})

    assert help_["range"] == ["0..250"]
    assert help_["default"] == "0"
    assert "length" not in help_
    assert "pattern" not in help_
