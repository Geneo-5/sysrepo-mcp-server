# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

"""
Sessions and notifications.

A session is what makes state survive between two requests: without one there
is nothing to attach a notification subscription to, and nothing to hold the
events that arrive between two polls. The two subjects are tested together
because neither means anything without the other.

The notification used throughout is oven-ready, from oven.yang. It is emitted
with sr_notif_send rather than by waiting for the plugin to heat the oven,
so the tests are deterministic.
"""

import time

import pytest

from .conftest import wait_for

OVEN_READY = "/oven:oven-ready"


# ---------------------------------------------------------------------------
# Creating a session
# ---------------------------------------------------------------------------


def test_initialize_returns_a_session_header(mcp):
    response = mcp.initialize()

    assert response.status == 200
    assert response.headers.get("mcp-session-id"), (
        "initialize must return the session identifier in the "
        "Mcp-Session-Id header"
    )


def test_session_identifiers_are_unique(mcp):
    seen = set()

    for _ in range(5):
        client = mcp.open_session()
        seen.add(client.session_id)
        client.close_session()

    assert len(seen) == 5


def test_session_identifier_is_not_guessable(mcp):
    # Not a statistical test, only a check that the identifier is long and
    # not a counter. A predictable one lets anyone use another agent's
    # session.
    client = mcp.open_session()

    try:
        assert len(client.session_id) >= 16
        assert client.session_id.isalnum()
        assert not client.session_id.isdigit()
    finally:
        client.close_session()


def test_a_session_survives_between_requests(session):
    first = session.tool("get_status", {})
    second = session.tool("get_status", {})

    assert first["version"] == second["version"]


def test_the_session_is_counted_while_it_lives(session):
    status = session.tool("get_status", {"verbose": True})
    identifiers = {entry["session_id"] for entry in status["sessions"]}

    assert session.session_id in identifiers
    assert status["active_sessions"] >= 1


def test_the_session_knows_which_one_is_current(session):
    status = session.tool("get_status", {"verbose": True})
    current = [entry for entry in status["sessions"] if entry["current"]]

    assert len(current) == 1
    assert current[0]["session_id"] == session.session_id


# ---------------------------------------------------------------------------
# Using and losing a session
# ---------------------------------------------------------------------------


def test_an_unknown_session_identifier_is_rejected(mcp):
    response = mcp.request(
        b'{"jsonrpc":"2.0","id":1,"method":"ping"}',
        extra_headers={"Mcp-Session-Id": "0123456789abcdef0123456789abcdef"},
    )

    # 404 is what tells a client to re-initialize rather than retry.
    assert response.status == 404


def test_a_deleted_session_is_gone(mcp):
    client = mcp.open_session()

    assert client.close_session().status == 204

    response = client.rpc("ping")

    assert response.status == 404


def test_deleting_twice_reports_the_session_as_gone(mcp):
    client = mcp.open_session()

    assert client.close_session().status == 204
    assert client.close_session().status == 404


def test_delete_without_a_session_is_rejected(mcp):
    response = mcp.request(body=None, method="DELETE", content_type=None)

    assert response.status == 404


def test_requests_without_a_session_still_work(mcp):
    # The session identifier is optional for tools that keep no state: an
    # agent doing a one-shot read should not have to handshake first.
    assert mcp.tool("get_status", {})["version"]


def test_a_session_is_released_on_delete(mcp):
    before = mcp.tool("get_status", {})["active_sessions"]

    client = mcp.open_session()
    during = mcp.tool("get_status", {})["active_sessions"]
    client.close_session()

    after = mcp.tool("get_status", {})["active_sessions"]

    assert during == before + 1
    assert after == before


def test_two_sessions_do_not_see_each_other(mcp_oven):
    first = mcp_oven.open_session()
    second = mcp_oven.open_session()

    try:
        first.tool("sr_notif_subscribe", {"module": "oven"})

        assert first.tool("sr_notif_list_subscriptions", {})["subscriptions"]
        assert second.tool("sr_notif_list_subscriptions", {})["subscriptions"] == []
    finally:
        first.close_session()
        second.close_session()


# ---------------------------------------------------------------------------
# Notification tools require a session
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "tool",
    [
        "sr_notif_subscribe",
        "sr_notif_unsubscribe",
        "sr_notif_list_subscriptions",
        "sr_notif_poll",
    ],
)
def test_notification_tools_refuse_a_sessionless_request(mcp, tool):
    error = mcp.tool_error(tool, {"module": "oven"})

    assert error["code"] == -32008, (
        "a tool that keeps state between requests must say a session is "
        "needed, not fail generically"
    )
    assert "initialize" in error["data"]["detail"]


def test_sending_a_notification_needs_no_session(mcp_oven):
    # sr_notif_send keeps no state between requests, so it works without a
    # session. Checked here because the other notification tools do not.
    assert mcp_oven.tool("sr_notif_send", {"xpath": OVEN_READY})["ok"] is True


# ---------------------------------------------------------------------------
# Subscribing
# ---------------------------------------------------------------------------


def test_subscribe_returns_an_identifier(oven_session):
    result = oven_session.tool("sr_notif_subscribe", {"module": "oven"})

    assert isinstance(result["subscription_id"], int)
    assert result["subscription_id"] >= 1
    assert result["module"] == "oven"
    assert result["session_id"] == oven_session.session_id


def test_subscribe_requires_a_module(oven_session):
    error = oven_session.tool_error("sr_notif_subscribe", {})

    assert error["code"] == -32602


def test_subscribe_to_an_unknown_module_fails(oven_session):
    error = oven_session.tool_error(
        "sr_notif_subscribe", {"module": "no-such-module"}
    )

    assert error["code"] in (-32003, -32602)


def test_subscribe_rejects_a_malformed_filter(oven_session):
    error = oven_session.tool_error(
        "sr_notif_subscribe", {"module": "oven", "xpath": "oven-ready"}
    )

    assert error["code"] == -32602


def test_subscriptions_are_listed(oven_session):
    first = oven_session.tool("sr_notif_subscribe", {"module": "oven"})
    second = oven_session.tool(
        "sr_notif_subscribe", {"module": "oven", "xpath": OVEN_READY}
    )

    listed = oven_session.tool("sr_notif_list_subscriptions", {})
    identifiers = {entry["subscription_id"] for entry in listed["subscriptions"]}

    assert identifiers == {first["subscription_id"], second["subscription_id"]}


def test_unsubscribe_removes_one_subscription(oven_session):
    first = oven_session.tool("sr_notif_subscribe", {"module": "oven"})
    oven_session.tool("sr_notif_subscribe", {"module": "oven"})

    removed = oven_session.tool(
        "sr_notif_unsubscribe", {"subscription_id": first["subscription_id"]}
    )

    assert removed["removed"] == 1

    remaining = oven_session.tool("sr_notif_list_subscriptions", {})["subscriptions"]

    assert len(remaining) == 1
    assert remaining[0]["subscription_id"] != first["subscription_id"]


def test_unsubscribe_without_an_id_removes_everything(oven_session):
    oven_session.tool("sr_notif_subscribe", {"module": "oven"})
    oven_session.tool("sr_notif_subscribe", {"module": "oven"})

    removed = oven_session.tool("sr_notif_unsubscribe", {})

    assert removed["removed"] == 2
    assert oven_session.tool("sr_notif_list_subscriptions", {})["subscriptions"] == []


def test_unsubscribe_an_unknown_id_is_not_found(oven_session):
    error = oven_session.tool_error(
        "sr_notif_unsubscribe", {"subscription_id": 9999}
    )

    assert error["code"] == -32003


# ---------------------------------------------------------------------------
# Receiving
# ---------------------------------------------------------------------------


def poll_until_any(client, timeout=10.0):
    """Poll until at least one notification arrives, and return the batch."""
    return wait_for(
        lambda: (lambda r: r if r["notifications"] else None)(
            client.tool("sr_notif_poll", {})
        ),
        timeout=timeout,
    )


def test_poll_is_empty_before_anything_happens(oven_session):
    result = oven_session.tool("sr_notif_poll", {})

    assert result["notifications"] == []
    assert result["pending"] == 0
    assert result["dropped"] == 0


def test_a_sent_notification_is_received(oven_session):
    oven_session.tool("sr_notif_subscribe", {"module": "oven"})
    oven_session.tool("sr_notif_send", {"xpath": OVEN_READY})

    batch = poll_until_any(oven_session)

    assert batch, "the notification never arrived"

    notification = batch["notifications"][0]

    assert notification["xpath"] == OVEN_READY
    assert notification["kind"] == "realtime"
    assert notification["timestamp"] > 0
    assert "oven:oven-ready" in notification["data"]


def test_polling_drains_the_queue(oven_session):
    oven_session.tool("sr_notif_subscribe", {"module": "oven"})
    oven_session.tool("sr_notif_send", {"xpath": OVEN_READY})

    assert poll_until_any(oven_session)

    # The same event must not be handed out twice.
    again = oven_session.tool("sr_notif_poll", {})

    assert again["notifications"] == []
    assert again["pending"] == 0


def test_peek_leaves_the_queue_alone(oven_session):
    oven_session.tool("sr_notif_subscribe", {"module": "oven"})
    oven_session.tool("sr_notif_send", {"xpath": OVEN_READY})

    peeked = wait_for(
        lambda: (lambda r: r if r["notifications"] else None)(
            oven_session.tool("sr_notif_poll", {"peek": True})
        )
    )

    assert peeked
    assert peeked["pending"] >= 1

    drained = oven_session.tool("sr_notif_poll", {})

    assert drained["notifications"], "peek must not consume the queue"


def test_several_notifications_arrive_in_order(oven_session):
    oven_session.tool("sr_notif_subscribe", {"module": "oven"})

    for _ in range(3):
        oven_session.tool("sr_notif_send", {"xpath": OVEN_READY})

    received = []
    deadline = time.monotonic() + 10.0

    while len(received) < 3 and time.monotonic() < deadline:
        received += oven_session.tool("sr_notif_poll", {})["notifications"]
        time.sleep(0.2)

    assert len(received) >= 3

    timestamps = [entry["timestamp"] for entry in received[:3]]

    assert timestamps == sorted(timestamps)


def test_max_limits_the_batch_size(oven_session):
    oven_session.tool("sr_notif_subscribe", {"module": "oven"})

    for _ in range(3):
        oven_session.tool("sr_notif_send", {"xpath": OVEN_READY})

    # Give the events a chance to be queued, then take one at a time.
    wait_for(
        lambda: (lambda r: r if r["pending"] >= 2 else None)(
            oven_session.tool("sr_notif_poll", {"peek": True})
        )
    )

    batch = oven_session.tool("sr_notif_poll", {"max": 1})

    assert len(batch["notifications"]) == 1
    assert batch["returned"] == 1
    assert batch["pending"] >= 1


def test_negative_max_is_invalid_params(oven_session):
    error = oven_session.tool_error("sr_notif_poll", {"max": -1})

    assert error["code"] == -32602


def test_counters_are_reported(oven_session):
    oven_session.tool("sr_notif_subscribe", {"module": "oven"})
    oven_session.tool("sr_notif_send", {"xpath": OVEN_READY})

    assert poll_until_any(oven_session)

    result = oven_session.tool("sr_notif_poll", {})

    assert result["total_received"] >= 1
    assert result["dropped"] == 0


def test_subscription_counter_tracks_what_it_received(oven_session):
    oven_session.tool("sr_notif_subscribe", {"module": "oven"})
    oven_session.tool("sr_notif_send", {"xpath": OVEN_READY})

    assert poll_until_any(oven_session)

    listed = oven_session.tool("sr_notif_list_subscriptions", {})

    assert listed["subscriptions"][0]["received"] >= 1


def test_nothing_is_received_without_a_subscription(oven_session):
    oven_session.tool("sr_notif_send", {"xpath": OVEN_READY})
    time.sleep(1.0)

    assert oven_session.tool("sr_notif_poll", {})["notifications"] == []


def test_a_matching_filter_delivers(oven_session):
    # A filter naming the notification itself must let it through; a
    # subscription that filters out everything it was asked to watch would be
    # indistinguishable from one that is simply broken.
    oven_session.tool(
        "sr_notif_subscribe", {"module": "oven", "xpath": OVEN_READY}
    )
    oven_session.tool("sr_notif_send", {"xpath": OVEN_READY})

    batch = poll_until_any(oven_session)

    assert batch
    assert batch["notifications"][0]["xpath"] == OVEN_READY


def test_unsubscribing_stops_delivery(oven_session):
    oven_session.tool("sr_notif_subscribe", {"module": "oven"})
    oven_session.tool("sr_notif_unsubscribe", {})

    oven_session.tool("sr_notif_send", {"xpath": OVEN_READY})
    time.sleep(1.0)

    assert oven_session.tool("sr_notif_poll", {})["notifications"] == []


def test_a_new_session_starts_with_an_empty_queue(mcp_oven):
    first = mcp_oven.open_session()

    try:
        first.tool("sr_notif_subscribe", {"module": "oven"})
        first.tool("sr_notif_send", {"xpath": OVEN_READY})
        assert poll_until_any(first)
    finally:
        first.close_session()

    second = mcp_oven.open_session()

    try:
        assert second.tool("sr_notif_poll", {})["notifications"] == []
    finally:
        second.close_session()


# ---------------------------------------------------------------------------
# Sending
# ---------------------------------------------------------------------------


def test_sending_an_unknown_notification_is_not_found(oven_session):
    error = oven_session.tool_error(
        "sr_notif_send", {"xpath": "/oven:no-such-notification"}
    )

    assert error["code"] == -32003


def test_sending_requires_an_xpath(oven_session):
    error = oven_session.tool_error("sr_notif_send", {})

    assert error["code"] == -32602


def test_sending_rejects_a_malformed_xpath(oven_session):
    error = oven_session.tool_error("sr_notif_send", {"xpath": "oven-ready"})

    assert error["code"] == -32602


# ---------------------------------------------------------------------------
# The whole flow
# ---------------------------------------------------------------------------


def test_full_notification_session(mcp_oven):
    """
    What an agent watching a device actually does: open a session, subscribe,
    do its work, come back for what happened, then clean up.
    """
    client = mcp_oven.open_session()

    try:
        # 1. Subscribe to the oven's notifications.
        subscription = client.tool("sr_notif_subscribe", {"module": "oven"})

        assert subscription["subscription_id"] >= 1

        # 2. Nothing has happened yet.
        assert client.tool("sr_notif_poll", {})["notifications"] == []

        # 3. Something happens.
        client.tool("sr_notif_send", {"xpath": OVEN_READY})

        # 4. The agent comes back and collects it.
        batch = poll_until_any(client)

        assert batch and len(batch["notifications"]) == 1
        assert batch["notifications"][0]["xpath"] == OVEN_READY

        # 5. And the queue is empty again.
        assert client.tool("sr_notif_poll", {})["pending"] == 0

        # 6. Clean up.
        assert client.tool("sr_notif_unsubscribe", {})["removed"] == 1
    finally:
        assert client.close_session().status == 204

    # 7. The session is gone for good.
    assert client.rpc("ping").status == 404
