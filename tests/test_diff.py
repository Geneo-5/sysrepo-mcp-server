# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

"""
End-to-end tests for `sr_diff_config`: the changes between two datastore
subtrees.

The diff is computed by libyang (`lyd_diff_siblings`) over the two subtrees read
from the `source` and `destination` datastores, then flattened into one entry
per change. Each entry carries `{xpath, operation, value, previous_value}`
where `operation` is one of `created`, `replaced`, `deleted`. A leaf is one
entry; a created or deleted subtree (a list entry, say) is ONE entry for its
topmost node, not one per descendant.

`LYD_DIFF_DEFAULTS` tells libyang to compare default nodes instead of ignoring
them. A leaf moving between its implicit default and an explicit value is
therefore a create or a delete, and two distinct explicit values is a replace.
The oven model has two leaves to exercise it: `turned-on` (bool, default
false) and `temperature` (uint8, range 0..250, default 0).

Writing a leaf explicitly, even with its default value, stores an explicit
node. To put a leaf on its *implicit* default the tests therefore delete it
(`_reset`) instead of writing the default back.

Every test that touches the oven sets `running` AND `candidate` itself: the
`oven_off` fixture only rewinds `running`, and `oven_diff` clears `candidate`
afterwards, so a test's edits never leak into the next one.
"""

import pytest

# Root of the oven, as an absolute xpath with its module prefix.
OVEN = "/oven:oven"

# The two configuration leaves of the oven.
TEMPERATURE = "/oven:oven/temperature"
TURNED_ON = "/oven:oven/turned-on"

# A list entry of the project's test module, used to exercise a created or
# deleted subtree (the oven only has leaves).
API_KEY = "test-diff-key-0001"
API_KEY_LIST = "/sysrepo-mcp-test:api-key"
API_KEY_ENTRY = f"{API_KEY_LIST}[key='{API_KEY}']"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _reset(client, datastore):
    """Put both oven leaves back on their implicit defaults in `datastore`."""
    for xpath in (TEMPERATURE, TURNED_ON):
        client.delete_config(xpath, datastore=datastore)


def _set(client, datastore, *, turned_on=None, temperature=None):
    """
    Bring `datastore` to a known state: both leaves on their implicit default,
    then the given ones written explicitly.
    """
    _reset(client, datastore)

    leaves = {}
    if turned_on is not None:
        leaves["turned-on"] = turned_on
    if temperature is not None:
        leaves["temperature"] = temperature

    if leaves:
        client.edit_config({"oven:oven": leaves}, datastore=datastore)


def _values(node):
    """Every scalar found in a JSON value, recursively."""
    if isinstance(node, dict):
        return [v for child in node.values() for v in _values(child)]
    if isinstance(node, list):
        return [v for child in node for v in _values(child)]
    return [node]


def _entry(diff, xpath):
    """The single diff entry at exactly `xpath`, or fail loudly."""
    matches = [e for e in diff["diff"] if e.get("xpath") == xpath]
    assert len(matches) == 1, (
        f"expected exactly one entry at {xpath}, got {diff['diff']}"
    )
    return matches[0]


def _assert_changes(diff, expected):
    """
    The diff holds exactly the `expected` {(operation, xpath)} pairs: nothing
    missing, nothing extra, no duplicates, and `changed` agrees.
    """
    actual = [(e["operation"], e["xpath"]) for e in diff["diff"]]

    assert sorted(actual) == sorted(expected), f"unexpected diff: {diff['diff']}"
    assert diff["changed"] == len(expected)


@pytest.fixture
def oven_diff(oven_off):
    """`oven_off`, with `candidate` cleared again once the test is over."""
    yield oven_off
    _reset(oven_off, "candidate")


# ---------------------------------------------------------------------------
# Leaf-level changes
# ---------------------------------------------------------------------------


def test_no_change(oven_diff):
    """Two identical subtrees diff to nothing."""
    _set(oven_diff, "running", turned_on=False, temperature=0)
    _set(oven_diff, "candidate", turned_on=False, temperature=0)

    diff = oven_diff.diff("running", "candidate", xpath=OVEN)

    assert diff["changed"] == 0
    assert diff["diff"] == []


def test_no_change_on_implicit_defaults(oven_diff):
    """Two subtrees that both sit on their implicit defaults diff to nothing."""
    _set(oven_diff, "running")
    _set(oven_diff, "candidate")

    diff = oven_diff.diff("running", "candidate", xpath=OVEN)

    assert diff["changed"] == 0
    assert diff["diff"] == []


def test_created(oven_diff):
    """A leaf that moves from its implicit default to a value is created."""
    _set(oven_diff, "running")
    _set(oven_diff, "candidate", temperature=200)

    diff = oven_diff.diff("running", "candidate", xpath=OVEN)

    _assert_changes(diff, [("created", TEMPERATURE)])

    entry = _entry(diff, TEMPERATURE)
    assert 200 in _values(entry["value"])
    assert "previous_value" not in entry


def test_deleted(oven_diff):
    """A leaf that moves from a value back to its implicit default is deleted."""
    _set(oven_diff, "running", temperature=100)
    _set(oven_diff, "candidate")

    diff = oven_diff.diff("running", "candidate", xpath=OVEN)

    _assert_changes(diff, [("deleted", TEMPERATURE)])

    entry = _entry(diff, TEMPERATURE)
    assert 100 in _values(entry["previous_value"])
    assert "value" not in entry


def test_replaced(oven_diff):
    """Two distinct explicit values on the same leaf is a replace."""
    _set(oven_diff, "running", temperature=100)
    _set(oven_diff, "candidate", temperature=200)

    diff = oven_diff.diff("running", "candidate", xpath=OVEN)

    _assert_changes(diff, [("replaced", TEMPERATURE)])

    entry = _entry(diff, TEMPERATURE)
    assert 200 in _values(entry["value"])
    assert 100 in _values(entry["previous_value"])


def test_created_boolean_leaf(oven_diff):
    """`turned-on` moving from its implicit default (false) to true is created."""
    _set(oven_diff, "running")
    _set(oven_diff, "candidate", turned_on=True)

    diff = oven_diff.diff("running", "candidate", xpath=OVEN)

    _assert_changes(diff, [("created", TURNED_ON)])
    assert True in _values(_entry(diff, TURNED_ON)["value"])


def test_replaced_boolean_keeps_its_type(oven_diff):
    """
    The previous value of a replaced boolean is reported with its real type,
    not re-parsed from libyang's canonical string ("true" / "false").
    """
    _set(oven_diff, "running", turned_on=True)
    _set(oven_diff, "candidate", turned_on=False)

    diff = oven_diff.diff("running", "candidate", xpath=OVEN)

    _assert_changes(diff, [("replaced", TURNED_ON)])

    entry = _entry(diff, TURNED_ON)
    assert True in _values(entry["previous_value"])
    assert False in _values(entry["value"])


def test_every_changed_leaf_is_reported(oven_diff):
    """Sibling leaves changed together all show up, not just the first."""
    _set(oven_diff, "running", turned_on=False, temperature=100)
    _set(oven_diff, "candidate", turned_on=True, temperature=200)

    diff = oven_diff.diff("running", "candidate", xpath=OVEN)

    _assert_changes(
        diff,
        [("replaced", TEMPERATURE), ("replaced", TURNED_ON)],
    )


def test_xpath_restricts_the_diff(oven_diff):
    """A leaf xpath only diffs that leaf, and is echoed back."""
    _set(oven_diff, "running", turned_on=False, temperature=100)
    _set(oven_diff, "candidate", turned_on=True, temperature=200)

    diff = oven_diff.diff("running", "candidate", xpath=TEMPERATURE)

    _assert_changes(diff, [("replaced", TEMPERATURE)])
    assert diff["xpath"] == TEMPERATURE


def test_without_xpath_the_whole_datastore_is_diffed(oven_diff):
    """No xpath filter reads the whole datastore; the result says so."""
    _set(oven_diff, "running", temperature=100)
    _set(oven_diff, "candidate", temperature=200)

    diff = oven_diff.diff("running", "candidate")

    assert diff["xpath"] == "data"
    assert ("replaced", TEMPERATURE) in [
        (e["operation"], e["xpath"]) for e in diff["diff"]
    ]


def test_result_names_the_datastores(oven_diff):
    """The result names the two datastores it was called with."""
    _set(oven_diff, "running", temperature=100)
    _set(oven_diff, "candidate", temperature=200)

    diff = oven_diff.diff("running", "candidate", xpath=OVEN)

    assert diff["source"] == "running"
    assert diff["target"] == "candidate"
    assert diff["changed"] == len(diff["diff"])


# ---------------------------------------------------------------------------
# Created and deleted subtrees
# ---------------------------------------------------------------------------


def test_created_and_deleted_list_entry(mcp_module):
    """
    A list entry present on one side only is ONE created (or deleted) entry
    for the entry itself, carrying the whole subtree: its descendants have no
    metadata of their own and must not be dropped nor reported separately.
    """
    client = mcp_module

    client.edit_config(
        {"sysrepo-mcp-test:api-key": [{"key": API_KEY, "user": "admin"}]},
        datastore="candidate",
    )
    try:
        created = client.diff("running", "candidate", xpath=API_KEY_LIST)

        _assert_changes(created, [("created", API_KEY_ENTRY)])

        values = _values(_entry(created, API_KEY_ENTRY)["value"])
        assert API_KEY in values
        assert "admin" in values
        assert "previous_value" not in _entry(created, API_KEY_ENTRY)

        deleted = client.diff("candidate", "running", xpath=API_KEY_LIST)

        _assert_changes(deleted, [("deleted", API_KEY_ENTRY)])

        entry = _entry(deleted, API_KEY_ENTRY)
        assert API_KEY in _values(entry["previous_value"])
        assert "admin" in _values(entry["previous_value"])
        assert "value" not in entry
    finally:
        client.delete_config(API_KEY_ENTRY, datastore="candidate")


# ---------------------------------------------------------------------------
# Rejected arguments
# ---------------------------------------------------------------------------


def test_identical_datastores_rejected(oven_diff):
    """Diffing a datastore against itself is a param error, not a diff."""
    err = oven_diff.tool_error("sr_diff_config", {
        "source": "running",
        "destination": "running",
        "xpath": OVEN,
    })

    assert err["code"] == -32602


@pytest.mark.parametrize(
    "arguments",
    [
        {"xpath": OVEN},
        {"source": "running", "xpath": OVEN},
        {"destination": "candidate", "xpath": OVEN},
        {"source": "running", "destination": "nvram", "xpath": OVEN},
        {"source": "nvram", "destination": "candidate", "xpath": OVEN},
        {"source": 1, "destination": "candidate", "xpath": OVEN},
        {"source": "running", "destination": "candidate", "xpath": OVEN,
         "max_depth": -1},
    ],
    ids=[
        "no-datastores",
        "no-destination",
        "no-source",
        "unknown-destination",
        "unknown-source",
        "source-not-a-string",
        "negative-max-depth",
    ],
)
def test_invalid_arguments_rejected(oven_diff, arguments):
    """A missing, unknown or malformed argument is a param error."""
    err = oven_diff.tool_error("sr_diff_config", arguments)

    assert err["code"] == -32602
