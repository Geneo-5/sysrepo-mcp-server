# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

"""
End-to-end tests for `sr_diff_config`: the leaf-level changes between two
datastore subtrees.

The diff is computed by libyang (`lyd_diff_siblings`) over the two subtrees read
from the running and candidate datastores, then flattened into one entry per
leaf-level change. Each entry carries `{xpath, operation, value,
previous_value}` where `operation` is one of `created`, `replaced`, `deleted`.

`libyang` is told to skip default values (`LYD_DIFF_DEFAULTS`), so a leaf moving
between its default and a real value is a create or delete, and two distinct
non-default values is a replace. The oven model has two leaves to exercise it:
`turned-on` (bool) and `temperature` (uint8, range 0..250).

Every test resets `candidate` explicitly: the `oven_off` fixture only rewinds
`running`, so a test's edits to `candidate` would otherwise leak into the next.
"""

# The two configuration leaves of the oven, as libyang reports them in a diff.
OVEN = "/oven:oven"

# The two configuration leaves of the oven, as libyang reports them in a diff.
TEMPERATURE = "/oven:oven/temperature"
TURNED_ON = "/oven:oven/turned-on"


def _by_operation(diff, operation):
    """Return the diff entries of a given operation, by xpath."""
    return [e for e in diff["diff"] if e.get("operation") == operation]


def _leaf(diff, operation, xpath):
    """Find the single diff entry of `operation` at `xpath`, or fail loudly."""
    matches = _by_operation(diff, operation)
    matches = [e for e in matches if e.get("xpath", "").endswith(xpath)]
    assert len(matches) == 1, (
        f"expected exactly one {operation} entry at {xpath}, got {matches}"
    )
    return matches[0]


def _set(client, turned_on, temperature, datastore="candidate"):
    """Set the candidate (or running) oven to a known state."""
    client.edit_config(
        {"oven:oven": {"turned-on": turned_on, "temperature": temperature}},
        datastore=datastore,
    )


def test_no_change(oven_off):
    """Two identical subtrees diff to nothing."""
    _set(oven_off, False, 0)

    diff = oven_off.diff("running", "candidate")

    assert diff["changed"] == 0
    assert diff["diff"] == []
    assert diff["source"] == "running"
    assert diff["target"] == "candidate"


def test_created(oven_off):
    """A leaf that moves from its default to a real value is created."""
    _set(oven_off, False, 0)        # running: base
    _set(oven_off, True, 200)       # candidate: temperature 200, turned on

    diff = oven_off.diff("running", "candidate")

    entry = _leaf(diff, "created", TEMPERATURE)
    assert entry["xpath"] == TEMPERATURE
    assert "value" in entry
    assert "previous_value" not in entry


def test_deleted(oven_off):
    """A leaf that moves from a real value back to its default is deleted."""
    _set(oven_off, False, 100)      # running: temperature 100
    _set(oven_off, True, 0)         # candidate: base

    diff = oven_off.diff("running", "candidate")

    entry = _leaf(diff, "deleted", TEMPERATURE)
    assert entry["xpath"] == TEMPERATURE
    assert "previous_value" in entry
    assert "value" not in entry


def test_replaced(oven_off):
    """Two distinct non-default values on the same leaf is a replace."""
    _set(oven_off, False, 100)      # running: temperature 100
    _set(oven_off, True, 200)       # candidate: temperature 200

    diff = oven_off.diff("running", "candidate")
    entry = _leaf(diff, "replaced", TEMPERATURE)
    assert entry["xpath"] == TEMPERATURE
    assert "value" in entry
    assert "previous_value" in entry


def test_created_leaf(oven_off):
    """`turned-on` flips from its default true-state: a created leaf too."""
    _set(oven_off, False, 0)        # running: off
    _set(oven_off, True, 0)         # candidate: on

    diff = oven_off.diff("running", "candidate")

    entry = _leaf(diff, "created", TURNED_ON)
    assert entry["xpath"] == TURNED_ON


def test_identical_datastores_rejected(oven_off):
    """Diffing a datastore against itself is a param error, not a diff."""
    _set(oven_off, True, 200)

    err = oven_off.tool_error("sr_diff_config", {
        "source": "running",
        "destination": "running",
        "xpath": OVEN,
    })

    assert err["code"] == -32602


def test_empty_config(oven_off):
    """A missing `source`/`destination` is a param error."""
    err = oven_off.tool_error("sr_diff_config", {"xpath": OVEN})

    assert err["code"] == -32602


def test_source_and_target(oven_off):
    """The result names the two datastores it was called with."""
    _set(oven_off, False, 0)
    _set(oven_off, True, 200)

    diff = oven_off.diff("running", "candidate")

    assert diff["source"] == "running"
    assert diff["target"] == "candidate"
