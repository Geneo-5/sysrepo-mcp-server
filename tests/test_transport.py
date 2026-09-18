# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

"""
HTTP and JSON-RPC framing.

These tests never call a tool. They check that the layer below the tools
behaves: the right status codes, the right headers, and an answer to every
request. A FastCGI responder that finishes a request without writing anything
leaves the client waiting for a timeout, which is the failure mode these tests
exist to catch.
"""

import json

import pytest

from .conftest import MCP_ENDPOINT


# ---------------------------------------------------------------------------
# HTTP method and content type
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("method", ["GET", "PUT", "HEAD", "PATCH"])
def test_only_post_is_accepted(mcp, method):
    response = mcp.request(body=None, method=method, content_type=None)

    assert response.status == 405


def test_delete_without_a_session_is_not_allowed(mcp):
    # DELETE is a valid method on the endpoint, but it only means something
    # for a session: without one there is nothing to terminate.
    response = mcp.request(body=None, method="DELETE", content_type=None)

    assert response.status == 404


def test_method_not_allowed_advertises_what_is_allowed(mcp):
    response = mcp.request(body=None, method="GET", content_type=None)
    allow = response.headers.get("allow", "").lower()

    assert response.status == 405
    assert "post" in allow
    assert "delete" in allow


def test_missing_content_type_is_rejected(mcp):
    response = mcp.request(b'{"jsonrpc":"2.0","id":1,"method":"ping"}', content_type=None)

    assert response.status == 415


def test_wrong_content_type_is_rejected(mcp):
    response = mcp.request(
        b'{"jsonrpc":"2.0","id":1,"method":"ping"}', content_type="text/plain"
    )

    assert response.status == 415


def test_content_type_with_charset_is_accepted(mcp):
    response = mcp.request(
        b'{"jsonrpc":"2.0","id":1,"method":"ping"}',
        content_type="application/json; charset=utf-8",
    )

    assert response.status == 200


# ---------------------------------------------------------------------------
# Body handling
# ---------------------------------------------------------------------------


def test_empty_body_is_rejected(mcp):
    response = mcp.request(b"")

    assert response.status == 400


def test_oversized_body_is_rejected(mcp):
    # One byte past the documented 1 MiB ceiling.
    padding = "x" * (1024 * 1024 + 1)
    body = json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": "ping", "params": {"pad": padding}}
    ).encode()

    response = mcp.request(body)

    assert response.status == 413


def test_body_at_the_limit_is_accepted(mcp):
    # Comfortably under the ceiling: the point is that a large-but-legal body
    # is read whole, not truncated.
    padding = "x" * (512 * 1024)
    body = json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": "ping", "params": {"pad": padding}}
    ).encode()

    response = mcp.request(body)

    assert response.status == 200
    assert response.json()["id"] == 1


# ---------------------------------------------------------------------------
# JSON-RPC framing
# ---------------------------------------------------------------------------


def test_malformed_json_is_a_parse_error(mcp):
    response = mcp.request(b"{ this is not json")

    assert response.status == 200
    assert response.json()["error"]["code"] == -32700


def test_truncated_json_is_a_parse_error(mcp):
    response = mcp.request(b'{"jsonrpc":"2.0","id":1,"method":')

    assert response.status == 200
    assert response.json()["error"]["code"] == -32700


@pytest.mark.parametrize("body", [b"[1,2,3]", b'"a string"', b"42", b"null"])
def test_non_object_payload_is_an_invalid_request(mcp, body):
    response = mcp.request(body)

    assert response.status == 200
    assert response.json()["error"]["code"] in (-32600, -32700)


def test_missing_method_is_an_invalid_request(mcp):
    response = mcp.request(b'{"jsonrpc":"2.0","id":7}')

    assert response.status == 200

    payload = response.json()

    assert payload["error"]["code"] == -32600
    assert payload["id"] == 7


def test_non_string_method_is_an_invalid_request(mcp):
    response = mcp.request(b'{"jsonrpc":"2.0","id":8,"method":42}')

    assert response.status == 200
    assert response.json()["error"]["code"] == -32600


def test_unknown_method_is_method_not_found(mcp):
    error = mcp.error("no/such/method")

    assert error["code"] == -32601


# ---------------------------------------------------------------------------
# Notifications
#
# A JSON-RPC notification has no id and must never receive a JSON-RPC
# response. Answering one is a specification violation that confuses a client
# into matching the answer with an unrelated request.
# ---------------------------------------------------------------------------


def test_notification_gets_no_json_rpc_response(mcp):
    response = mcp.rpc("notifications/initialized", req_id=None)

    assert response.status == 202
    assert response.body == b""


def test_unknown_notification_is_still_not_answered(mcp):
    response = mcp.rpc("notifications/whatever", req_id=None)

    assert response.status == 202
    assert response.body == b""


# ---------------------------------------------------------------------------
# Response shape
# ---------------------------------------------------------------------------


def test_response_is_json_with_a_correct_content_length(mcp):
    response = mcp.rpc("ping")

    assert response.status == 200
    assert "application/json" in response.headers["content-type"]
    assert int(response.headers["content-length"]) == len(response.body)


def test_response_is_not_cached(mcp):
    response = mcp.rpc("ping")

    assert "no-store" in response.headers.get("cache-control", "")


@pytest.mark.parametrize("req_id", [1, 4242, "abc", "a-uuid-like-string"])
def test_request_id_is_echoed_unchanged(mcp, req_id):
    response = mcp.rpc("ping", req_id=req_id)

    assert response.status == 200

    payload = response.json()

    assert payload["id"] == req_id
    assert payload["jsonrpc"] == "2.0"


def test_ping_returns_an_empty_result(mcp):
    assert mcp.result("ping") == {}


# ---------------------------------------------------------------------------
# Endpoint routing
# ---------------------------------------------------------------------------


def test_unknown_path_is_not_the_mcp_endpoint(mcp):
    # Anything outside the FastCGI mapping is served by lighttpd, not by
    # sysrepo-mcp. A 200 here would mean the endpoint is mounted too broadly.
    response = mcp.request(
        b'{"jsonrpc":"2.0","id":1,"method":"ping"}', path="/not" + MCP_ENDPOINT
    )

    assert response.status != 200


# ---------------------------------------------------------------------------
# Robustness
# ---------------------------------------------------------------------------


def test_server_survives_a_burst_of_requests(mcp):
    # max-procs is 1 in the test configuration, so this also checks that the
    # accept loop keeps going rather than serving one request and stopping.
    for index in range(50):
        response = mcp.rpc("ping", req_id=index)

        assert response.status == 200
        assert response.json()["id"] == index


def test_server_survives_a_malformed_request_in_the_middle(mcp):
    assert mcp.rpc("ping").status == 200

    mcp.request(b"{{{ broken")

    assert mcp.rpc("ping").status == 200
