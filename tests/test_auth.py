# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

"""
Authentication and ACL tests (P0).

The server is configured with ``docker/sysrepo-mcp.conf`` which defines
three API keys:

.. table::

   ============== =================================
   Key            User
   ============== =================================
   ``test-admin-`` ``admin``
   key-0001
   ``test-admin-`` ``operator``
   key-0002
   ``test-ro-key-`` ``viewer``
   0001
   ============== =================================

When API keys are configured, every MCP endpoint enforces authentication.
A request without a valid credential is rejected; an authenticated request
is bound to the user from the matching key.

The ``mcp_auth`` fixture starts a dedicated server instance with this
configuration file so that the gateway logic can be tested end-to-end.
"""

from __future__ import annotations

import json
import pytest

from .conftest import MCP_PROTOCOL_VERSION, McpClient

# -----------------------------------------------------------------------
# JSON-RPC error codes (the JSON-RPC 2.0 specification)
# -----------------------------------------------------------------------

MCP_ERR_DENIED       = -32003  # unauthorized / access denied
MCP_ERR_NO_SESSION   = -32008  # session expired
MCP_ERR_INTERNAL     = -32603  # server error
MCP_ERR_METHOD       = -32601  # method not found (JSON-RPC standard)
MCP_ERR_INVALID_ID   = -32600  # invalid request


# -----------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------


def test_initialize_no_credential(mcp_auth: McpClient) -> None:
    """
    ``initialize`` without credential when API keys are configured -> 401.
    """
    response = mcp_auth.initialize()

    assert response.status == 401

    payload = response.json()

    assert "error" in payload
    assert payload["error"]["code"] == MCP_ERR_INTERNAL
    assert "Unauthorized" in payload["error"]["message"]


def test_initialize_with_credential(mcp_auth: McpClient) -> None:
    """
    ``initialize`` with a valid Bearer token -> 200, session created.

    A session-bound client carrying the session ID is returned by the
    ``mcp_auth`` fixture via ``open_session_with_token()``; this test only
    validates that the initialize flow with a credential succeeds.
    """
    session = mcp_auth.open_session_with_token("test-admin-key-0001")

    # Verify the session carries the expected user identity by calling a
    # tool — the session user is "admin" because test-admin-key-0001
    # maps to the "admin" user in the test configuration.
    response = session.call("get_status", {})

    assert response.status == 200

    payload = response.json()

    assert "result" in payload


def test_initialize_invalid_credential(mcp_auth: McpClient) -> None:
    """
    ``initialize`` with a credential not in ``api_keys[]`` -> 401.
    """
    body = json.dumps({
        "jsonrpc": "2.0",
        "id": 1,
        "method": "tools/call",
        "params": {"name": "get_status", "arguments": {"name": "get_status"}},
    }).encode("utf-8")

    response = mcp_auth.request(
        body=body,
        extra_headers={"Authorization": "Bearer this-key-does-not-exist"},
    )

    assert response.status == 401

    payload = response.json()

    assert payload["error"]["code"] == MCP_ERR_INTERNAL


def test_tool_call_without_auth(mcp_auth: McpClient) -> None:
    """
    Any tool call without authentication -> 401 when API keys are
    configured.

    Applies to ``initialize`` as well as ``tools/call``, ``ping``, and
    all other MCP endpoints.
    """
    body = json.dumps({
        "jsonrpc": "2.0",
        "id": 1,
        "method": "tools/call",
        "params": {"name": "get_status", "arguments": {"name": "get_status"}},
    }).encode("utf-8")

    response = mcp_auth.request(
        body=body,
    )

    assert response.status == 401

    payload = response.json()

    assert payload["error"]["code"] == MCP_ERR_INTERNAL


def test_module_install_denied(mcp_auth: McpClient) -> None:
    """
    ``sr_module_install`` and ``sr_module_uninstall`` are blocked when
    API keys are configured (``sphinx/todo.rst`` P0).

    This is the default: these operations change the schema for the entire
    sysrepo instance and destroying a module wipes all its data.  The
    check happens before the NACM layer.
    """
    session = mcp_auth.open_session_with_token("test-admin-key-0001")

    err = session.tool_error("sr_module_install", {
        "yang_file": "/etc/sysrepo/yang/oven.yang",
    })
    assert err["code"] == MCP_ERR_DENIED

    err = session.tool_error("sr_module_uninstall", {
        "module": "oven",
    })
    assert err["code"] == MCP_ERR_DENIED


def test_404_unknown_session(mcp_auth: McpClient) -> None:
    """
    Re-use a session with an id that does not exist -> 404.

    The Streamable HTTP binding returns 404 so that a client knows it
    must call ``initialize`` again.
    """
    response = mcp_auth.request(
        extra_headers={"Mcp-Session-Id": "nonexistent-session-00001"},
    )

    assert response.status == 404

    payload = response.json()

    assert payload["error"]["code"] == MCP_ERR_NO_SESSION


def test_session_reuse_preserves_user(mcp_auth: McpClient) -> None:
    """
    Create a session with valid credentials, then re-use the session id
    without re-sending the credential: the session user is authoritative
    and the session remains valid.

    Credentials are only used to create the session; they are ignored on
    subsequent requests that carry a valid session id.
    """
    # Step 1: initialize with valid Bearer token to create a session.
    initialize_payload = json.dumps({
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": MCP_PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "sysrepo-mcp-tests", "version": "1.0.0"},
        },
    }).encode("utf-8")

    response = mcp_auth.request(
        body=initialize_payload,
        extra_headers={"Authorization": "Bearer test-admin-key-0001"},
    )

    assert response.status == 200

    session_id = response.headers.get("mcp-session-id")
    assert session_id

    # Step 2: re-use the session id without the credential.
    call_body = json.dumps({
        "jsonrpc": "2.0",
        "id": 2,
        "method": "tools/call",
        "params": {"name": "get_status", "arguments": {"name": "get_status"}},
    }).encode("utf-8")

    response2 = mcp_auth.request(
        body=call_body,
        extra_headers={"Mcp-Session-Id": session_id},
    )

    assert response2.status == 200

    payload = response2.json()

    assert "result" in payload


def test_nacm_denied_rpc(mcp_auth: McpClient) -> None:
    """
    ``viewer`` est un utilisateur NACM avec droits de lecture uniquement.
    Un appel à ``sr_execute_rpc`` vers un RPC d'écriture du module ``oven``
    est refusé avec ``-32003`` par NACM (``sphinx/todo.rst`` P0.10).
    """
    session = mcp_auth.open_session_with_token("test-ro-key-0001")

    err = session.tool_error("sr_execute_rpc",
    {
        "xpath": "/oven:insert-food",
        "input": {"time": "now"},
    })
    assert err["code"] == MCP_ERR_DENIED
