# SPDX-License-Identifier: LGPL-3.0-only

"""Standalone FastCGI listener tests, without a web-server supervisor."""

from __future__ import annotations

import json
import os
import shutil
import socket
import struct
import subprocess
import stat
import time
from pathlib import Path

import pytest

PROJECT_ROOT = Path(__file__).resolve().parent.parent


FCGI_BEGIN_REQUEST = 1
FCGI_END_REQUEST = 3
FCGI_PARAMS = 4
FCGI_STDIN = 5
FCGI_STDOUT = 6
FCGI_STDERR = 7


def _record(record_type: int, content: bytes, request_id: int = 1) -> bytes:
    padding = (-len(content)) % 8
    return (
        struct.pack(">BBHHBB", 1, record_type, request_id, len(content), padding, 0)
        + content
        + (b"\0" * padding)
    )


def _pair(name: str, value: str) -> bytes:
    name_bytes = name.encode()
    value_bytes = value.encode()
    return (
        bytes([len(name_bytes), len(value_bytes)])
        + name_bytes
        + value_bytes
    )


def _read_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("FastCGI responder closed the socket early")
        data.extend(chunk)
    return bytes(data)


def _request(sock: socket.socket) -> dict:
    body = json.dumps({
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": "2025-01-13",
            "capabilities": {},
            "clientInfo": {"name": "standalone-test", "version": "1"},
        },
    }).encode()
    params = b"".join(
        _pair(name, value)
        for name, value in {
            "REQUEST_METHOD": "POST",
            "CONTENT_TYPE": "application/json",
            "CONTENT_LENGTH": str(len(body)),
            "SCRIPT_NAME": "/mcp",
            "REQUEST_URI": "/mcp",
            "SERVER_PROTOCOL": "HTTP/1.1",
        }.items()
    )
    begin = struct.pack(">HB5s", 1, 0, b"\0" * 5)
    sock.sendall(
        _record(FCGI_BEGIN_REQUEST, begin)
        + _record(FCGI_PARAMS, params)
        + _record(FCGI_PARAMS, b"")
        + _record(FCGI_STDIN, body)
        + _record(FCGI_STDIN, b"")
    )

    output = bytearray()
    while True:
        header = _read_exact(sock, 8)
        version, record_type, request_id, content_length, padding_length, _ = (
            struct.unpack(">BBHHBB", header)
        )
        content = _read_exact(sock, content_length)
        if padding_length:
            _read_exact(sock, padding_length)
        assert version == 1 and request_id == 1
        if record_type == FCGI_STDOUT:
            output.extend(content)
        elif record_type == FCGI_STDERR:
            continue
        elif record_type == FCGI_END_REQUEST:
            break

    headers, response_body = bytes(output).split(b"\r\n\r\n", 1)
    status = next(
        int(line.split(b":", 1)[1].split()[0])
        for line in headers.split(b"\r\n")
        if line.startswith(b"Status:")
    )
    assert status == 200, bytes(output)
    return json.loads(response_body)


def _free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@pytest.mark.parametrize("mode", ["unix", "tcp"])
def test_standalone_fastcgi_listener(mode: str, tmp_path: Path) -> None:
    binary = Path(os.environ.get(
        "SYSREPO_MCP_BIN", str(PROJECT_ROOT / "build" / "src" / "sysrepo-mcp")
    ))
    if not binary.is_file():
        pytest.skip(f"{binary} not found: build it with scripts/build-docker.sh")

    repo = tmp_path / "repository"
    repo.mkdir()
    socket_path = tmp_path / "sysrepo-mcp.sock"
    port = _free_tcp_port()
    bind = (
        f'unix_socket_path = "{socket_path}";'
        if mode == "unix"
        else f'tcp_host = "127.0.0.1"; tcp_port = {port};'
    )
    config = tmp_path / "sysrepo-mcp.conf"
    config.write_text(
        "server: { transport: { mode = \"" + mode + "\"; " + bind + " }; "
        "log: { syslog_enabled = false; console = false; file = \"\"; }; };\n"
    )

    env = dict(os.environ)
    env["SYSREPO_REPOSITORY_PATH"] = str(repo)
    env["SYSREPO_SHM_PREFIX"] = f"sr_mcp_standalone_{os.getpid()}_{mode}"
    env.setdefault("LD_LIBRARY_PATH", "/usr/local/lib")
    proc = subprocess.Popen(
        [str(binary), "--config", str(config)],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    address = (
        (socket.AF_UNIX, str(socket_path))
        if mode == "unix"
        else (socket.AF_INET, ("127.0.0.1", port))
    )

    try:
        deadline = time.monotonic() + 20
        payload = None
        last_error: OSError | None = None
        while time.monotonic() < deadline and proc.poll() is None:
            try:
                with socket.socket(address[0], socket.SOCK_STREAM) as client:
                    client.settimeout(3)
                    client.connect(address[1])
                    if mode == "unix":
                        assert stat.S_IMODE(socket_path.stat().st_mode) == 0o660
                    payload = _request(client)
                    break
            except OSError as exc:
                last_error = exc
                time.sleep(0.1)
        if payload is None:
            output = proc.stdout.read().decode(errors="replace") if proc.poll() is not None else ""
            pytest.fail(f"standalone {mode} listener did not answer: {last_error}\n{output}")
        assert payload["result"]["protocolVersion"] == "2025-01-13"
        assert payload["result"]["serverInfo"]["name"] == "sysrepo-mcp"
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    if mode == "unix":
        assert not socket_path.exists()
