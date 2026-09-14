# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

"""
Integration tests for sysrepo-mcp using the oven YANG model.

These tests verify that the MCP server correctly handles requests
for the oven example module from extern/sysrepo/examples/plugin/oven.yang.

The oven module contains:
- container oven: configuration (turned-on: boolean, temperature: uint8)
- container oven-state: operational state (temperature: uint8, food-inside: boolean)
- rpc insert-food: input time (enum: now, on-oven-ready)
- rpc remove-food: no input
- notification oven-ready
"""

import json
import os
import sys
import tempfile
import subprocess
import time
import requests
from pathlib import Path
from typing import Optional, Dict, Any

import pytest


# =============================================================================
# Test configuration
# =============================================================================

# Path to the built binary (relative to project root)
BINARY_PATH = Path("build/sysrepo-mcp")

# Oven YANG module path (relative to project root)
OVEN_YANG_PATH = Path("extern/sysrepo/examples/plugin/oven.yang")

# Sysrepo datastore directory for testing
TEST_DATASTORE_DIR = "/tmp/sysrepo-mcp-test-datastore"

# FastCGI socket path for testing
TEST_FCGI_SOCKET = "/tmp/sysrepo-mcp-test.sock"


# =============================================================================
# Test fixtures
# =============================================================================

@pytest.fixture(scope="module")
def setup_test_environment():
    """Setup test environment - ensure datastore directory exists."""
    os.makedirs(TEST_DATASTORE_DIR, exist_ok=True)
    yield
    # Cleanup
    if os.path.exists(TEST_FCGI_SOCKET):
        try:
            os.unlink(TEST_FCGI_SOCKET)
        except:
            pass


@pytest.fixture(scope="module")
def lighttpd_process(setup_test_environment):
    """Start lighttpd with FastCGI proxy to sysrepo-mcp."""
    # This fixture would start lighttpd in a real test environment
    # For now, we'll use direct FastCGI communication in tests
    yield None


# =============================================================================
# Helper functions
# =============================================================================

def send_fcgi_request(socket_path: str, request_data: Dict[str, Any]) -> Dict[str, Any]:
    """
    Send a JSON-RPC request to a FastCGI socket and return the response.
    
    This is a simplified implementation for testing. In production, this would
    go through a reverse proxy like lighttpd or nginx.
    """
    # Convert request to JSON string
    request_json = json.dumps(request_data)
    
    # FCGI request format:
    # Version, type, requestId, contentLength, paddingLength
    # Followed by request body
    import struct
    
    # FCGI header
    FCGI_VERSION_1 = 1
    FCGI_BEGIN_REQUEST = 1
    FCGI_RESPONDER = 1
    FCGI_NULL_REQUEST_ID = 0
    
    # Request body
    params = {
        'GATEWAY_INTERFACE': 'CGI/1.1',
        'REQUEST_METHOD': 'POST',
        'CONTENT_TYPE': 'application/json',
        'CONTENT_LENGTH': str(len(request_json)),
        'SCRIPT_NAME': '/mcp',
        'SCRIPT_FILENAME': '/mcp',
        'QUERY_STRING': '',
        'REQUEST_URI': '/mcp',
        'SERVER_NAME': 'localhost',
        'SERVER_PORT': '80',
        'SERVER_PROTOCOL': 'HTTP/1.1',
    }
    
    # Build FCGI params
    params_data = b''
    for name, value in params.items():
        name_bytes = name.encode('utf-8')
        value_bytes = value.encode('utf-8')
        params_data += struct.pack('!HH', len(name_bytes), len(value_bytes)) + name_bytes + value_bytes
    
    params_data += b'\x00\x00\x00\x00'  # Empty name/value to terminate
    
    # Build FCGI stdin
    stdin_data = request_json.encode('utf-8')
    stdin_data += b'\x00' * ((8 - (len(stdin_data) % 8)) % 8)  # Padding
    
    # Content lengths
    params_length = len(params_data)
    stdin_length = len(stdin_data)
    
    # Build BEGIN_REQUEST record
    begin_record = struct.pack(
        '!BBHHHxx',
        FCGI_VERSION_1,
        FCGI_BEGIN_REQUEST,
        FCGI_NULL_REQUEST_ID,
        0,  # role: FCGI_RESPONDER
        0   # flags
    )
    
    # Build PARAMS record
    params_record = struct.pack(
        '!BBHHHxx',
        FCGI_VERSION_1,
        FCGI_PARAMS,
        FCGI_NULL_REQUEST_ID,
        params_length,
        0
    ) + params_data
    params_padding = b'\x00' * ((8 - (len(params_data) % 8)) % 8)
    
    # Build STDIN record
    stdin_record = struct.pack(
        '!BBHHHxx',
        FCGI_VERSION_1,
        FCGI_STDIN,
        FCGI_NULL_REQUEST_ID,
        stdin_length,
        0
    ) + stdin_data
    stdin_padding = b'\x00' * ((8 - (len(stdin_data) % 8)) % 8)
    
    # Build END_REQUEST record
    end_record = struct.pack(
        '!BBHHHxx',
        FCGI_VERSION_1,
        FCGI_END_REQUEST,
        FCGI_NULL_REQUEST_ID,
        0,
        0
    )
    
    # Send all records
    import socket
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(socket_path)
        sock.sendall(begin_record + params_record + params_padding + 
                     stdin_record + stdin_padding + end_record)
        
        # Read response
        response_data = b''
        while True:
            header = sock.recv(8)
            if len(header) != 8:
                break
            version, record_type, request_id, content_length, padding_length = \
                struct.unpack('!BBHHHxx', header)
            
            if record_type == FCGI_END_REQUEST:
                break
                
            content = sock.recv(content_length)
            response_data += content
            
            # Skip padding
            if padding_length > 0:
                sock.recv(padding_length)
        
        sock.close()
        
        # Parse response
        return json.loads(response_data.decode('utf-8'))
    except Exception as e:
        return {"jsonrpc": "2.0", "error": {"code": -32000, "message": str(e)}}


def call_mcp_tool(socket_path: str, tool_name: str, arguments: Optional[Dict] = None) -> Dict[str, Any]:
    """
    Call an MCP tool via FastCGI.
    
    Args:
        socket_path: Path to FastCGI socket
        tool_name: Name of the MCP tool to call
        arguments: Dictionary of arguments for the tool
    
    Returns:
        JSON-RPC response as a dictionary
    """
    request = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "tools/call",
        "params": {
            "name": tool_name,
            "arguments": arguments or {}
        }
    }
    return send_fcgi_request(socket_path, request)


# =============================================================================
# Basic server tests
# =============================================================================

class TestServerBasics:
    """Test basic server functionality via FastCGI socket."""

    @pytest.fixture(autouse=True)
    def check_server_available(self):
        """Check if the test server socket is available."""
        socket_path = "/tmp/sysrepo-mcp-test.sock"
        if not os.path.exists(socket_path):
            pytest.skip("Server not running (socket not found)")

    def test_server_version(self):
        """Test that server returns correct version."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "get_status",
                "arguments": {}
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response
        assert "version" in response["result"]
        assert "0.1" in response["result"]["version"]

    def test_server_help(self):
        """Test that server responds to get_status."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "get_status",
                "arguments": {}
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response
        assert "sysrepo-mcp" in response["result"].get("version", "") or \
               "version" in response["result"]


# =============================================================================
# Oven configuration tests
# =============================================================================

class TestOvenConfiguration:
    """Test MCP tools with oven YANG module configuration operations."""

    @pytest.fixture(autouse=True)
    def check_server_available(self):
        """Check if the test server socket is available."""
        socket_path = "/tmp/sysrepo-mcp-test.sock"
        if not os.path.exists(socket_path):
            pytest.skip("Server not running (socket not found)")

    def test_get_oven_config_full(self):
        """Test getting full oven configuration."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "sr_get_config",
                "arguments": {
                    "xpath": "/oven:oven"
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response
        assert "data" in response["result"]

    def test_get_oven_temperature(self):
        """Test getting specific oven temperature."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "sr_get_config",
                "arguments": {
                    "xpath": "/oven:oven/temperature"
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response

    def test_edit_oven_temperature(self):
        """Test setting oven temperature."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "sr_edit_config",
                "arguments": {
                    "xpath": "/oven:oven",
                    "target": "running",
                    "config": {
                        "oven:oven": {
                            "temperature": 180
                        }
                    }
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response

    def test_edit_oven_turned_on(self):
        """Test turning oven on/off."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "sr_edit_config",
                "arguments": {
                    "xpath": "/oven:oven",
                    "target": "running",
                    "config": {
                        "oven:oven": {
                            "turned-on": True
                        }
                    }
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response


# =============================================================================
# Oven operational state tests
# =============================================================================

class TestOvenOperationalState:
    """Test MCP tools with oven YANG module operational state."""

    @pytest.fixture(autouse=True)
    def check_server_available(self):
        """Check if the test server socket is available."""
        socket_path = "/tmp/sysrepo-mcp-test.sock"
        if not os.path.exists(socket_path):
            pytest.skip("Server not running (socket not found)")

    def test_get_oven_state(self):
        """Test getting oven operational state."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "sr_get_operational",
                "arguments": {
                    "xpath": "/oven:oven-state"
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response

    def test_get_oven_state_temperature(self):
        """Test getting oven state temperature only."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "sr_get_operational",
                "arguments": {
                    "xpath": "/oven:oven-state/temperature"
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response


# =============================================================================
# Oven RPC tests
# =============================================================================

class TestOvenRPC:
    """Test MCP tools with oven YANG module RPC operations."""

    @pytest.fixture(autouse=True)
    def check_server_available(self):
        """Check if the test server socket is available."""
        socket_path = "/tmp/sysrepo-mcp-test.sock"
        if not os.path.exists(socket_path):
            pytest.skip("Server not running (socket not found)")

    def test_execute_insert_food_now(self):
        """Test executing insert-food RPC with time=now."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "sr_execute_rpc",
                "arguments": {
                    "xpath": "/oven:insert-food",
                    "input_params": {
                        "time": "now"
                    }
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response

    def test_execute_insert_food_on_ready(self):
        """Test executing insert-food RPC with time=on-oven-ready."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "sr_execute_rpc",
                "arguments": {
                    "xpath": "/oven:insert-food",
                    "input_params": {
                        "time": "on-oven-ready"
                    }
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response

    def test_execute_remove_food(self):
        """Test executing remove-food RPC."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "sr_execute_rpc",
                "arguments": {
                    "xpath": "/oven:remove-food"
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response


# =============================================================================
# YANG tree and help tests
# =============================================================================

class TestYANGTree:
    """Test YANG schema exploration tools with oven module."""

    @pytest.fixture(autouse=True)
    def check_server_available(self):
        """Check if the test server socket is available."""
        socket_path = "/tmp/sysrepo-mcp-test.sock"
        if not os.path.exists(socket_path):
            pytest.skip("Server not running (socket not found)")

    def test_get_tree_oven_module(self):
        """Test getting full oven schema tree."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "get_tree",
                "arguments": {
                    "module": "oven",
                    "xpath": "/"
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response

    def test_get_tree_oven_container(self):
        """Test getting oven container subtree."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "get_tree",
                "arguments": {
                    "module": "oven",
                    "xpath": "/oven:oven"
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response

    def test_get_help_oven_temperature(self):
        """Test getting help for oven temperature leaf."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "get_help",
                "arguments": {
                    "xpath": "/oven:oven/temperature"
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response

    def test_get_help_oven_container(self):
        """Test getting help for oven container."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "get_help",
                "arguments": {
                    "xpath": "/oven:oven"
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response

    def test_get_help_insert_food_rpc(self):
        """Test getting help for insert-food RPC."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "get_help",
                "arguments": {
                    "xpath": "/oven:insert-food"
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "result" in response


# =============================================================================
# Error handling tests
# =============================================================================

class TestErrorHandling:
    """Test error handling for invalid requests."""

    @pytest.fixture(autouse=True)
    def check_server_available(self):
        """Check if the test server socket is available."""
        socket_path = "/tmp/sysrepo-mcp-test.sock"
        if not os.path.exists(socket_path):
            pytest.skip("Server not running (socket not found)")

    def test_missing_xpath_sr_get_config(self):
        """Test error when xpath is missing for sr_get_config."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "sr_get_config",
                "arguments": {}
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "error" in response

    def test_invalid_module_get_tree(self):
        """Test error when module doesn't exist for get_tree."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "get_tree",
                "arguments": {
                    "module": "nonexistent"
                }
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "error" in response

    def test_invalid_tool_name(self):
        """Test error when tool name doesn't exist."""
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "invalid_tool",
                "arguments": {}
            }
        }
        response = send_fcgi_request(TEST_FCGI_SOCKET, request)
        assert response["jsonrpc"] == "2.0"
        assert "error" in response


# =============================================================================
# Test data
# =============================================================================

# Expected oven configuration structure (from oven.yang)
OVEN_CONFIG_STRUCTURE = {
    "oven:oven": {
        "turned-on": False,
        "temperature": 0
    }
}

# Expected oven operational state structure
OVEN_STATE_STRUCTURE = {
    "oven:oven-state": {
        "temperature": 0,
        "food-inside": False
    }
}

# Expected get_tree response structure for oven
OVEN_TREE_STRUCTURE = {
    "tree": {
        "module": "oven",
        "namespace": "urn:sysrepo:oven",
        "prefix": "ov",
        "revision": "2018-01-19"
    },
    "nodes": [
        {"xpath": "/oven:oven", "type": "container"},
        {"xpath": "/oven:oven/turned-on", "type": "leaf"},
        {"xpath": "/oven:oven/temperature", "type": "leaf"},
        {"xpath": "/oven:oven-state", "type": "container"},
        {"xpath": "/oven:oven-state/temperature", "type": "leaf"},
        {"xpath": "/oven:oven-state/food-inside", "type": "leaf"}
    ],
    "references": []
}


# =============================================================================
# Utility test functions (for when server is running)
# =============================================================================

class TestOvenLive:
    """
    Live tests that require the server to be running.
    
    To run these tests:
    1. Start the server: ./build/sysrepo-mcp
    2. Configure a reverse proxy (lighttpd) to forward to the FastCGI socket
    3. Set the TEST_SERVER_URL environment variable
    4. Run: pytest tests/test_oven_integration.py::TestOvenLive -v
    """
    
    @pytest.fixture(autouse=True)
    def check_server_available(self):
        """Check if test server is available."""
        server_url = os.environ.get('TEST_SERVER_URL')
        if not server_url:
            pytest.skip("TEST_SERVER_URL not set - server not running")
        
        # Check if server responds
        try:
            response = requests.get(f"{server_url}/mcp", timeout=1)
        except:
            pytest.skip("Server not responding")
    
    def test_live_get_oven_config(self):
        """Live test: get oven configuration."""
        server_url = os.environ.get('TEST_SERVER_URL')
        
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "sr_get_config",
                "arguments": {
                    "xpath": "/oven:oven"
                }
            }
        }
        
        response = requests.post(
            f"{server_url}/mcp",
            json=request,
            headers={"Content-Type": "application/json"}
        )
        
        assert response.status_code == 200
        result = response.json()
        assert result["jsonrpc"] == "2.0"
        assert result["id"] == 1
        assert "result" in result
        assert "data" in result["result"]
        
        # Verify structure matches oven YANG
        data = result["result"]["data"]
        assert "oven:oven" in data
        oven = data["oven:oven"]
        assert "turned-on" in oven
        assert "temperature" in oven
    
    def test_live_get_oven_state(self):
        """Live test: get oven operational state."""
        server_url = os.environ.get('TEST_SERVER_URL')
        
        request = {
            "jsonrpc": "2.0",
            "id": 2,
            "method": "tools/call",
            "params": {
                "name": "sr_get_operational",
                "arguments": {
                    "xpath": "/oven:oven-state"
                }
            }
        }
        
        response = requests.post(
            f"{server_url}/mcp",
            json=request,
            headers={"Content-Type": "application/json"}
        )
        
        assert response.status_code == 200
        result = response.json()
        assert "result" in result
        assert "data" in result["result"]
        
        data = result["result"]["data"]
        assert "oven:oven-state" in data
    
    def test_live_get_tree_oven(self):
        """Live test: get oven schema tree."""
        server_url = os.environ.get('TEST_SERVER_URL')
        
        request = {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/call",
            "params": {
                "name": "get_tree",
                "arguments": {
                    "module": "oven",
                    "xpath": "/"
                }
            }
        }
        
        response = requests.post(
            f"{server_url}/mcp",
            json=request,
            headers={"Content-Type": "application/json"}
        )
        
        assert response.status_code == 200
        result = response.json()
        assert "result" in result
        assert "tree" in result["result"]
        assert "nodes" in result["result"]
    
    def test_live_execute_insert_food(self):
        """Live test: execute insert-food RPC."""
        server_url = os.environ.get('TEST_SERVER_URL')
        
        request = {
            "jsonrpc": "2.0",
            "id": 4,
            "method": "tools/call",
            "params": {
                "name": "sr_execute_rpc",
                "arguments": {
                    "xpath": "/oven:insert-food",
                    "input_params": {
                        "time": "now"
                    }
                }
            }
        }
        
        response = requests.post(
            f"{server_url}/mcp",
            json=request,
            headers={"Content-Type": "application/json"}
        )
        
        assert response.status_code == 200
        result = response.json()
        assert "result" in result
        assert "output" in result["result"]


# =============================================================================
# Test helper for documentation
# =============================================================================

if __name__ == "__main__":
    # Run with: python -m pytest tests/test_oven_integration.py -v
    pytest.main([__file__, "-v"])
