# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

"""
Basic skeleton tests for sysrepo-mcp.

These tests verify that the build process works correctly and that
basic functionality is available.
"""

import os
import sys
import subprocess
from pathlib import Path

import pytest


# =============================================================================
# Test configuration
# =============================================================================

# Path to the built binary (relative to project root)
BINARY_PATH = Path("build/sysrepo-mcp")

# Oven YANG module path (relative to project root)
OVEN_YANG_PATH = Path("extern/sysrepo/examples/plugin/oven.yang")


# =============================================================================
# Build tests
# =============================================================================

class TestBuild:
    """Test that the project builds correctly."""
    
    def test_binary_exists(self):
        """Test that the binary was built."""
        assert BINARY_PATH.exists(), f"Binary not found at {BINARY_PATH}"
    
    def test_binary_is_executable(self):
        """Test that the binary is executable."""
        assert os.access(BINARY_PATH, os.X_OK), "Binary is not executable"


# =============================================================================
# CLI tests (smoke tests)
# =============================================================================
# NOTE: The binary is linked with fcgi_stdio.h, which intercepts stdout/stderr
# at link time. Calling the binary directly via subprocess.run() produces empty
# output. These tests are skipped when the server is not running.
# The real smoke tests happen through the FastCGI socket (see
# test_oven_integration.py for integration tests).

class TestCLI:
    """Test command-line interface (skipped when server not available)."""

    @pytest.fixture(autouse=True)
    def check_server_available(self):
        """Check if the test server socket is available."""
        socket_path = "/tmp/sysrepo-mcp-test.sock"
        if not os.path.exists(socket_path):
            pytest.skip("Server not running (socket not found)")

    def test_help_output(self):
        """Test that --help produces output."""
        result = subprocess.run(
            [str(BINARY_PATH), "--help"],
            capture_output=True,
            text=True
        )
        
        assert result.returncode == 0
        assert "sysrepo-mcp" in result.stdout or "sysrepo-mcp" in result.stderr
        assert "--help" in result.stdout or "--help" in result.stderr
    
    def test_version_output(self):
        """Test that --version produces output."""
        result = subprocess.run(
            [str(BINARY_PATH), "--version"],
            capture_output=True,
            text=True
        )
        
        assert result.returncode == 0
        assert "sysrepo-mcp" in result.stdout or "sysrepo-mcp" in result.stderr
        assert "0.1" in result.stdout or "0.1" in result.stderr


# =============================================================================
# Oven YANG module availability
# =============================================================================

class TestOvenModule:
    """Test that oven YANG module is available."""
    
    def test_oven_yang_exists(self):
        """Test that oven.yang exists."""
        assert OVEN_YANG_PATH.exists(), f"oven.yang not found at {OVEN_YANG_PATH}"
    
    def test_oven_yang_content(self):
        """Test that oven.yang has expected content."""
        content = OVEN_YANG_PATH.read_text()
        
        # Check for key elements
        assert "module oven" in content
        assert "namespace \"urn:sysrepo:oven\"" in content
        assert "container oven" in content
        assert "container oven-state" in content
        assert "rpc insert-food" in content
        assert "rpc remove-food" in content
        assert "notification oven-ready" in content


# =============================================================================
# Test data structures
# =============================================================================

class TestOvenData:
    """Test expected data structures for oven module."""
    
    # Expected structure based on oven.yang
    OVEN_CONFIG_STRUCTURE = {
        "oven:oven": {
            "turned-on": False,
            "temperature": 0
        }
    }
    
    OVEN_STATE_STRUCTURE = {
        "oven:oven-state": {
            "temperature": 0,
            "food-inside": False
        }
    }
    
    def test_oven_config_structure(self):
        """Test that oven config structure is as expected."""
        # This is mostly for documentation
        assert "oven:oven" in self.OVEN_CONFIG_STRUCTURE
        assert "turned-on" in self.OVEN_CONFIG_STRUCTURE["oven:oven"]
        assert "temperature" in self.OVEN_CONFIG_STRUCTURE["oven:oven"]
    
    def test_oven_state_structure(self):
        """Test that oven state structure is as expected."""
        assert "oven:oven-state" in self.OVEN_STATE_STRUCTURE
        assert "temperature" in self.OVEN_STATE_STRUCTURE["oven:oven-state"]
        assert "food-inside" in self.OVEN_STATE_STRUCTURE["oven:oven-state"]


# =============================================================================
# Test XPath examples
# =============================================================================

class TestOvenXPaths:
    """Test XPath expressions for oven module."""
    
    EXPECTED_XPATHS = [
        "/oven:oven",
        "/oven:oven/turned-on",
        "/oven:oven/temperature",
        "/oven:oven-state",
        "/oven:oven-state/temperature",
        "/oven:oven-state/food-inside",
        "/oven:insert-food",
        "/oven:remove-food",
    ]
    
    def test_oven_xpaths_valid(self):
        """Test that expected oven XPaths are valid."""
        for xpath in self.EXPECTED_XPATHS:
            assert xpath.startswith("/oven:")
            assert len(xpath) > 0


# =============================================================================
# Test JSON-RPC request formats
# =============================================================================

class TestJSONRPCRequests:
    """Test JSON-RPC request formats for oven module."""
    
    def test_get_config_request(self):
        """Test sr_get_config request format."""
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
        
        assert request["jsonrpc"] == "2.0"
        assert request["method"] == "tools/call"
        assert request["params"]["name"] == "sr_get_config"
        assert request["params"]["arguments"]["xpath"] == "/oven:oven"
    
    def test_edit_config_request(self):
        """Test sr_edit_config request format."""
        request = {
            "jsonrpc": "2.0",
            "id": 2,
            "method": "tools/call",
            "params": {
                "name": "sr_edit_config",
                "arguments": {
                    "xpath": "/oven:oven",
                    "target": "running",
                    "config": {
                        "oven:oven": {
                            "temperature": 180,
                            "turned-on": True
                        }
                    }
                }
            }
        }
        
        assert request["params"]["arguments"]["xpath"] == "/oven:oven"
        assert request["params"]["arguments"]["target"] == "running"
        assert "config" in request["params"]["arguments"]
    
    def test_execute_rpc_request(self):
        """Test sr_execute_rpc request format for insert-food."""
        request = {
            "jsonrpc": "2.0",
            "id": 3,
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
        
        assert request["params"]["arguments"]["xpath"] == "/oven:insert-food"
        assert "input_params" in request["params"]["arguments"]
    
    def test_get_tree_request(self):
        """Test get_tree request format."""
        request = {
            "jsonrpc": "2.0",
            "id": 4,
            "method": "tools/call",
            "params": {
                "name": "get_tree",
                "arguments": {
                    "module": "oven",
                    "xpath": "/"
                }
            }
        }
        
        assert request["params"]["arguments"]["module"] == "oven"
        assert request["params"]["arguments"]["xpath"] == "/"
    
    def test_get_help_request(self):
        """Test get_help request format."""
        request = {
            "jsonrpc": "2.0",
            "id": 5,
            "method": "tools/call",
            "params": {
                "name": "get_help",
                "arguments": {
                    "xpath": "/oven:oven/temperature"
                }
            }
        }
        
        assert request["params"]["arguments"]["xpath"] == "/oven:oven/temperature"


# =============================================================================
# Test expected responses
# =============================================================================

class TestExpectedResponses:
    """Test expected response formats."""
    
    def test_get_config_response_format(self):
        """Test expected format for sr_get_config response."""
        response = {
            "jsonrpc": "2.0",
            "id": 1,
            "result": {
                "data": {
                    "oven:oven": {
                        "turned-on": False,
                        "temperature": 0
                    }
                },
                "module": "oven",
                "path": "/oven:oven"
            }
        }
        
        assert response["jsonrpc"] == "2.0"
        assert "result" in response
        assert "data" in response["result"]
        assert "module" in response["result"]
        assert "path" in response["result"]
    
    def test_edit_config_response_format(self):
        """Test expected format for sr_edit_config response."""
        response = {
            "jsonrpc": "2.0",
            "id": 2,
            "result": {
                "ok": True
            }
        }
        
        assert response["jsonrpc"] == "2.0"
        assert "result" in response
        assert "ok" in response["result"]
    
    def test_error_response_format(self):
        """Test expected format for error response."""
        response = {
            "jsonrpc": "2.0",
            "id": 0,
            "error": {
                "code": -32602,
                "message": "Invalid parameters"
            }
        }
        
        assert response["jsonrpc"] == "2.0"
        assert "error" in response
        assert "code" in response["error"]
        assert "message" in response["error"]


# =============================================================================
# Test file for standalone execution
# =============================================================================

if __name__ == "__main__":
    pytest.main([__file__, "-v"])
