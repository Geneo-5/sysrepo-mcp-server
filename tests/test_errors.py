# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

"""
Argument validation, error mapping, and the project's own YANG module.

An agent decides what to do next from the error alone, so an error has to say
which kind of failure it was. Collapsing everything onto -32603 would make
"your edit is wrong", worth retrying differently, indistinguishable from "the
server is broken", not worth retrying at all.

These tests also exercise yang/sysrepo-mcp.yang, which gives a list with a key
and therefore covers list handling, key predicates and empty matches. The oven
module has no list.
"""

import pytest

API_KEY = "/sysrepo-mcp:api-key"

# The key leaf is constrained to 16..256 characters.
KEY_A = "test-key-aaaaaaaaaa"
KEY_B = "test-key-bbbbbbbbbb"


# ---------------------------------------------------------------------------
# The project's own module
# ---------------------------------------------------------------------------


def test_project_module_installs(installed_modules):
    """
    yang/sysrepo-mcp.yang must be valid and installable.

    This is the only test that fails rather than skips when a module is
    missing: it is the project's own model, and an unusable one makes every
    runtime feature built on it unusable too.
    """
    result = installed_modules["sysrepo-mcp"]

    assert result.available, f"{result.path} is missing"
    assert result.installed, f"sysrepoctl refused the module:\n{result.output}"


@pytest.fixture
def clean_keys(mcp_module):
    """Remove every API key before and after a test."""
    client = mcp_module

    def reset():
        # sr_delete_config is the only tool that can remove data: a merge
        # cannot express a deletion, and a replace cannot express "nothing".
        client.call("sr_delete_config", {"xpath": API_KEY})

    reset()
    yield client
    reset()


def test_deleting_an_absent_node_succeeds_by_default(clean_keys):
    # The useful default for a cleanup: an agent should not have to check
    # whether something exists before removing it.
    assert clean_keys.delete_config(API_KEY)["ok"] is True


def test_strict_delete_of_an_absent_node_fails(clean_keys):
    error = clean_keys.tool_error(
        "sr_delete_config", {"xpath": API_KEY, "strict": True}
    )

    assert error["code"] in (-32003, -32005)


def test_delete_removes_one_list_entry(clean_keys):
    clean_keys.edit_config(
        {
            "sysrepo-mcp:api-key": [
                {"key": KEY_A, "user": "admin"},
                {"key": KEY_B, "user": "operator"},
            ]
        }
    )

    clean_keys.delete_config(f"{API_KEY}[key='{KEY_A}']")

    entries = clean_keys.get_config(API_KEY)["data"]["sysrepo-mcp:api-key"]

    assert [entry["key"] for entry in entries] == [KEY_B]


def test_delete_removes_the_whole_list(clean_keys):
    clean_keys.edit_config(
        {"sysrepo-mcp:api-key": [{"key": KEY_A, "user": "admin"}]}
    )

    clean_keys.delete_config(API_KEY)

    assert clean_keys.get_config(API_KEY)["data"] == {}


def test_delete_requires_an_xpath(mcp):
    error = mcp.tool_error("sr_delete_config", {})

    assert error["code"] == -32602


def test_empty_list_reads_as_empty_data(clean_keys):
    assert clean_keys.get_config(API_KEY)["data"] == {}


def test_a_list_entry_round_trips(clean_keys):
    clean_keys.edit_config(
        {"sysrepo-mcp:api-key": [{"key": KEY_A, "user": "admin"}]}
    )

    entries = clean_keys.get_config(API_KEY)["data"]["sysrepo-mcp:api-key"]

    assert entries == [{"key": KEY_A, "user": "admin"}]


def test_several_list_entries_are_returned_as_an_array(clean_keys):
    clean_keys.edit_config(
        {
            "sysrepo-mcp:api-key": [
                {"key": KEY_A, "user": "admin"},
                {"key": KEY_B, "user": "operator"},
            ]
        }
    )

    entries = clean_keys.get_config(API_KEY)["data"]["sysrepo-mcp:api-key"]

    assert isinstance(entries, list)
    assert {entry["key"] for entry in entries} == {KEY_A, KEY_B}


def test_a_key_predicate_selects_one_entry(clean_keys):
    clean_keys.edit_config(
        {
            "sysrepo-mcp:api-key": [
                {"key": KEY_A, "user": "admin"},
                {"key": KEY_B, "user": "operator"},
            ]
        }
    )

    selected = clean_keys.get_config(f"{API_KEY}[key='{KEY_A}']")
    entries = selected["data"]["sysrepo-mcp:api-key"]

    assert len(entries) == 1
    assert entries[0]["user"] == "admin"


def test_a_predicate_matching_nothing_returns_empty_data(clean_keys):
    result = clean_keys.get_config(f"{API_KEY}[key='no-such-key-000000']")

    assert result["data"] == {}


def test_a_list_entry_without_its_mandatory_leaf_is_refused(clean_keys):
    # user is mandatory: an entry without it must not reach the datastore.
    error = clean_keys.tool_error(
        "sr_edit_config",
        {"config": {"sysrepo-mcp:api-key": [{"key": KEY_A}]}},
    )

    assert error["code"] == -32005
    assert clean_keys.get_config(API_KEY)["data"] == {}


def test_a_key_shorter_than_the_schema_allows_is_refused(clean_keys):
    error = clean_keys.tool_error(
        "sr_edit_config",
        {"config": {"sysrepo-mcp:api-key": [{"key": "short", "user": "admin"}]}},
    )

    assert error["code"] == -32005


def test_server_state_is_not_writable(clean_keys):
    # server-state is config false; writing it must be refused rather than
    # silently ignored.
    error = clean_keys.tool_error(
        "sr_edit_config",
        {"config": {"sysrepo-mcp:server-state": {"version": "9.9.9"}}},
    )

    assert error["code"] in (-32002, -32005)


def test_schema_of_the_project_module_is_introspectable(mcp_module):
    tree = mcp_module.tool("get_tree", {"module": "sysrepo-mcp"})
    roots = tree["tree"]["nodes"]

    assert tree["tree"]["namespace"] == "urn:sysrepo-mcp:server"
    assert "api-key" in roots
    assert "server-state" in roots
    assert roots["api-key"]["type"] == "list"
    assert roots["server-state"]["config"] is False


def test_help_on_the_project_module(mcp_module):
    help_ = mcp_module.tool("get_help", {"xpath": f"{API_KEY}/user"})

    assert help_["node_type"] == "leaf"
    assert help_["mandatory"] is True
    assert help_["module"] == "sysrepo-mcp"


# ---------------------------------------------------------------------------
# Module listing
# ---------------------------------------------------------------------------


def test_list_modules_reports_the_installed_ones(mcp_oven):
    result = mcp_oven.tool("sr_list_modules", {})
    names = {entry["name"] for entry in result["modules"]}

    assert "oven" in names
    assert "ietf-netconf-acm" in names
    assert result["count"] == len(result["modules"])


def test_list_modules_describes_each_one(mcp_oven):
    modules = mcp_oven.tool("sr_list_modules", {})["modules"]
    oven = next(entry for entry in modules if entry["name"] == "oven")

    assert oven["revision"] == "2018-01-19"
    assert oven["namespace"] == "urn:sysrepo:oven"
    assert oven["prefix"] == "ov"
    assert oven["implemented"] is True


def test_list_modules_can_include_imported_ones(mcp_oven):
    implemented = mcp_oven.tool("sr_list_modules", {})["count"]
    everything = mcp_oven.tool(
        "sr_list_modules", {"implemented_only": False}
    )["count"]

    assert everything >= implemented


def test_module_install_requires_a_file(mcp):
    error = mcp.tool_error("sr_module_install", {})

    assert error["code"] == -32602


def test_module_uninstall_requires_a_module(mcp):
    error = mcp.tool_error("sr_module_uninstall", {})

    assert error["code"] == -32602


def test_uninstalling_an_unknown_module_fails(mcp):
    error = mcp.tool_error("sr_module_uninstall", {"module": "no-such-module"})

    assert error["code"] != -32603


# ---------------------------------------------------------------------------
# Error object shape
# ---------------------------------------------------------------------------


def test_error_object_has_a_code_and_a_message(mcp):
    error = mcp.tool_error("sr_get_config", {})

    assert isinstance(error["code"], int)
    assert isinstance(error["message"], str) and error["message"]


def test_error_object_explains_what_was_wrong(mcp):
    error = mcp.tool_error("sr_get_config", {})

    assert "xpath" in error["data"]["detail"].lower()


def test_sysrepo_failures_carry_the_original_message(mcp_oven):
    # A failure coming from sysrepo must keep sysrepo's own message: the
    # mapped code says the kind of failure, the message says which node.
    error = mcp_oven.tool_error(
        "sr_get_config", {"xpath": "this is not an xpath at all"}
    )

    assert "data" in error
    assert error["data"].get("detail")


def test_error_replaces_the_result_rather_than_joining_it(mcp):
    payload = mcp.call("sr_get_config", {}).json()

    assert "error" in payload
    assert "result" not in payload


# ---------------------------------------------------------------------------
# Missing and malformed arguments
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "tool",
    ["sr_get_config", "sr_delete_config", "sr_get_operational",
     "sr_execute_rpc", "sr_action", "sr_notif_send", "get_help"],
)
def test_missing_xpath_is_invalid_params(mcp, tool):
    error = mcp.tool_error(tool, {})

    assert error["code"] == -32602


def test_missing_config_is_invalid_params(mcp):
    error = mcp.tool_error("sr_edit_config", {})

    assert error["code"] == -32602


def test_missing_module_is_invalid_params(mcp):
    error = mcp.tool_error("get_tree", {})

    assert error["code"] == -32602


@pytest.mark.parametrize("value", [42, True, ["a"], {"a": 1}, None])
def test_non_string_xpath_is_invalid_params(mcp, value):
    error = mcp.tool_error("sr_get_config", {"xpath": value})

    assert error["code"] == -32602


@pytest.mark.parametrize("value", ["a string", 42, True, None])
def test_non_object_config_is_invalid_params(mcp, value):
    error = mcp.tool_error("sr_edit_config", {"config": value})

    assert error["code"] == -32602


def test_negative_max_depth_is_invalid_params(mcp):
    error = mcp.tool_error(
        "sr_get_config", {"xpath": "/oven:oven", "max_depth": -1}
    )

    assert error["code"] == -32602


# ---------------------------------------------------------------------------
# Datastore selection
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("datastore", ["running", "startup", "candidate"])
def test_documented_datastores_are_accepted(mcp_oven, datastore):
    result = mcp_oven.get_config("/oven:oven", datastore=datastore)

    assert result["datastore"] == datastore


@pytest.mark.parametrize("datastore", ["factory", "RUNNING", "", "operational "])
def test_unknown_datastore_is_invalid_params(mcp, datastore):
    error = mcp.tool_error(
        "sr_get_config", {"xpath": "/oven:oven", "datastore": datastore}
    )

    assert error["code"] == -32602


def test_operational_datastore_is_refused_by_the_config_reader(mcp):
    # Reading operational data through sr_get_config would bypass the
    # subscriber timeout that sr_get_operational applies.
    error = mcp.tool_error(
        "sr_get_config", {"xpath": "/oven:oven", "datastore": "operational"}
    )

    assert error["code"] == -32602
    assert "sr_get_operational" in error["data"]["detail"]


def test_operational_datastore_is_not_editable(mcp):
    error = mcp.tool_error(
        "sr_edit_config",
        {"config": {"oven:oven": {"temperature": 10}}, "datastore": "operational"},
    )

    assert error["code"] == -32602


@pytest.mark.parametrize("operation", ["delete", "create", "REPLACE", ""])
def test_unknown_edit_operation_is_invalid_params(mcp, operation):
    error = mcp.tool_error(
        "sr_edit_config",
        {"config": {"oven:oven": {"temperature": 10}}, "operation": operation},
    )

    assert error["code"] == -32602


# ---------------------------------------------------------------------------
# Malformed XPath
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "xpath",
    [
        "oven:oven",                 # not absolute
        "/oven",                     # no module prefix
        "/oven:oven[",               # unbalanced predicate
        "/nope:nope",                # unknown module
    ],
)
def test_malformed_xpath_is_rejected_clearly(mcp, xpath):
    error = mcp.tool_error("sr_get_config", {"xpath": xpath})

    # Whichever layer catches it, the answer must name a client-side problem
    # rather than an internal error.
    assert error["code"] in (-32602, -32003), (
        f"{xpath!r} reported as {error['code']}: {error.get('data')}"
    )
    assert error["code"] != -32603
