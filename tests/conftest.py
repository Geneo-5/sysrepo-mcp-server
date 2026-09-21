# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

"""
Shared fixtures for the sysrepo-mcp test suite.

Everything is exercised the way a real client would: HTTP requests to
lighttpd, which forwards them to sysrepo-mcp over FastCGI. The binary is never
invoked directly to probe its behaviour, since running a FastCGI responder from
a terminal only proves that it refuses to run from a terminal.

Isolation
---------
The suite never touches the system sysrepo repository. It points sysrepo at a
private repository and a private shared-memory prefix, both created under the
pytest temporary directory:

    SYSREPO_REPOSITORY_PATH   private repository, holding the YANG modules
    SYSREPO_SHM_PREFIX        private /dev/shm namespace

Both are passed to sysrepoctl, to the oven plugin process and, through
lighttpd's bin-environment, to sysrepo-mcp itself. A test run therefore leaves
no trace outside its temporary directory.

Layered fixtures
----------------
Each layer skips with an explicit reason rather than failing, so a partial
environment still runs the tests it can:

    sysrepo_env   -> private repository, needs sysrepoctl
    oven_module   -> oven.yang installed, needs extern/sysrepo
    mcp           -> lighttpd + sysrepo-mcp, needs the built binary
    oven_plugin   -> upstream oven plugin running, needs a C compiler
"""

from __future__ import annotations

import fcntl
import http.client
import json
import os
import shutil
import signal
import socket
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest

PROJECT_ROOT = Path(__file__).resolve().parent.parent

# The oven example ships with sysrepo, downloaded into extern/.
OVEN_YANG = PROJECT_ROOT / "extern" / "sysrepo" / "examples" / "plugin" / "oven.yang"
OVEN_PLUGIN_SRC = PROJECT_ROOT / "extern" / "sysrepo" / "examples" / "plugin" / "oven.c"

# Port 80 by default, as the deployment shape being tested is the real one.
# Docker sets net.ipv4.ip_unprivileged_port_start=0, so an unprivileged process
# in a container may bind it; override when that is not true.
TEST_PORT = int(os.environ.get("SYSREPO_MCP_TEST_PORT", "80"))
TEST_HOST = "127.0.0.1"

MCP_ENDPOINT = "/mcp"
MCP_PROTOCOL_VERSION = "2025-01-13"

STARTUP_TIMEOUT = 20.0
POLL_INTERVAL = 0.2


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------


def _wait_for_port(host: str, port: int, timeout: float) -> bool:
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(POLL_INTERVAL)

    return False


def _tail(path: Path, limit: int = 4000) -> str:
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return "(no log)"

    return text[-limit:]


@dataclass
class Response:
    """An HTTP response, kept whole so tests can assert on the status too."""

    status: int
    headers: dict[str, str]
    body: bytes

    def json(self) -> Any:
        return json.loads(self.body.decode("utf-8"))


# ---------------------------------------------------------------------------
# The client
# ---------------------------------------------------------------------------


class McpClient:
    """Minimal MCP client speaking plain HTTP to lighttpd."""

    def __init__(self, host: str, port: int, endpoint: str = MCP_ENDPOINT):
        self.host = host
        self.port = port
        self.endpoint = endpoint
        self.session_id: str | None = None
        self._next_id = 0

    # -- transport ------------------------------------------------------

    def request(
        self,
        body: bytes | None = None,
        method: str = "POST",
        content_type: str | None = "application/json",
        extra_headers: dict[str, str] | None = None,
        path: str | None = None,
    ) -> Response:
        """Send one raw HTTP request. Nothing is assumed about the answer."""
        headers: dict[str, str] = {"Accept": "application/json, text/event-stream"}

        if content_type is not None:
            headers["Content-Type"] = content_type
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        if extra_headers:
            headers.update(extra_headers)

        conn = http.client.HTTPConnection(self.host, self.port, timeout=30)
        try:
            conn.request(method, path or self.endpoint, body=body, headers=headers)
            raw = conn.getresponse()
            return Response(
                status=raw.status,
                headers={k.lower(): v for k, v in raw.getheaders()},
                body=raw.read(),
            )
        finally:
            conn.close()

    # -- sessions -------------------------------------------------------

    def initialize(self) -> Response:
        """Raw initialize, for tests that inspect the response themselves."""
        return self.rpc(
            "initialize",
            {
                "protocolVersion": MCP_PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "sysrepo-mcp-tests", "version": "1.0.0"},
            },
        )

    def open_session(self) -> "McpClient":
        """
        Run the handshake and return a client bound to the new session.

        A new client rather than a mutated one: the session-less client is
        shared by the whole run and must stay session-less.
        """
        response = self.initialize()

        assert response.status == 200, (
            f"initialize failed: HTTP {response.status} {response.body!r}"
        )

        session_id = response.headers.get("mcp-session-id")

        assert session_id, "initialize returned no Mcp-Session-Id header"

        client = McpClient(self.host, self.port, self.endpoint)
        client.session_id = session_id
        client.rpc("notifications/initialized", req_id=None)

        return client

    def close_session(self) -> Response:
        """Terminate the session this client is bound to."""
        return self.request(body=None, method="DELETE", content_type=None)

    # -- JSON-RPC -------------------------------------------------------

    def _allocate_id(self) -> int:
        self._next_id += 1
        return self._next_id

    def rpc(
        self,
        method: str,
        params: dict | None = None,
        req_id: int | None = -1,
    ) -> Response:
        """
        Send a JSON-RPC request.

        req_id=-1 allocates one, req_id=None sends a notification, anything
        else is used as-is so a test can check the id is echoed back.
        """
        message: dict[str, Any] = {"jsonrpc": "2.0", "method": method}

        if req_id == -1:
            message["id"] = self._allocate_id()
        elif req_id is not None:
            message["id"] = req_id
        if params is not None:
            message["params"] = params

        return self.request(json.dumps(message).encode("utf-8"))

    def result(self, method: str, params: dict | None = None) -> dict:
        """Send a request, require a JSON-RPC result, return it."""
        response = self.rpc(method, params)

        assert response.status == 200, f"HTTP {response.status}: {response.body!r}"

        payload = response.json()

        assert "error" not in payload, f"unexpected JSON-RPC error: {payload['error']}"
        assert "result" in payload, f"no result in {payload!r}"

        return payload["result"]

    def error(self, method: str, params: dict | None = None) -> dict:
        """Send a request, require a JSON-RPC error, return it."""
        response = self.rpc(method, params)

        assert response.status == 200, f"HTTP {response.status}: {response.body!r}"

        payload = response.json()

        assert "error" in payload, f"expected an error, got {payload!r}"

        return payload["error"]

    # -- tools ----------------------------------------------------------

    def call(self, name: str, arguments: dict | None = None) -> Response:
        """Raw tools/call, for tests that want to inspect the envelope."""
        params: dict[str, Any] = {"name": name}

        if arguments is not None:
            params["arguments"] = arguments

        return self.rpc("tools/call", params)

    def tool(self, name: str, arguments: dict | None = None) -> dict:
        """
        Call a tool and return its payload.

        Checks the MCP envelope on the way through: a result must carry a
        content array and must not be flagged isError.
        """
        response = self.call(name, arguments)

        assert response.status == 200, f"HTTP {response.status}: {response.body!r}"

        payload = response.json()

        assert "error" not in payload, (
            f"tool {name} failed: {payload['error']}"
        )

        result = payload["result"]

        assert result.get("isError") is False, f"tool {name} reported isError"
        assert isinstance(result.get("content"), list) and result["content"], (
            f"tool {name} returned no content block"
        )

        block = result["content"][0]

        assert block["type"] == "text"

        # The text block and structuredContent must describe the same thing;
        # a client may use either.
        assert json.loads(block["text"]) == result["structuredContent"]

        return result["structuredContent"]

    def tool_error(self, name: str, arguments: dict | None = None) -> dict:
        """Call a tool expected to fail, and return the JSON-RPC error."""
        response = self.call(name, arguments)

        assert response.status == 200, f"HTTP {response.status}: {response.body!r}"

        payload = response.json()

        assert "error" in payload, f"expected an error, got {payload!r}"

        return payload["error"]

    # -- debug output ---------------------------------------------------

    def _print_pipe(self, fd: Any, label: str) -> None:
        """Read and print everything available from a process pipe."""
        try:
            old_flags = fcntl.fcntl(fd.fileno(), fcntl.F_GETFL)
            fcntl.fcntl(fd.fileno(), fcntl.F_SETFL, old_flags | os.O_NONBLOCK)
            lines = b""
            while True:
                chunk = os.read(fd.fileno(), 65536)
                if not chunk:
                    break
                lines += chunk
        except BlockingIOError:
            pass  # nothing to read
        finally:
            fcntl.fcntl(fd.fileno(), fcntl.F_SETFL, old_flags)
        if lines:
            print(f"\n--- {label} ---\n{lines.decode(errors='replace')}", end="")

    # -- convenience ----------------------------------------------------</parameter>

    def get_config(self, xpath: str, **kwargs: Any) -> dict:
        return self.tool("sr_get_config", {"xpath": xpath, **kwargs})

    def edit_config(self, config: dict, **kwargs: Any) -> dict:
        return self.tool("sr_edit_config", {"config": config, **kwargs})

    def get_operational(self, xpath: str, **kwargs: Any) -> dict:
        return self.tool("sr_get_operational", {"xpath": xpath, **kwargs})

    def delete_config(self, xpath: str, **kwargs: Any) -> dict:
        return self.tool("sr_delete_config", {"xpath": xpath, **kwargs})


# ---------------------------------------------------------------------------
# Fixtures: sysrepo repository
# ---------------------------------------------------------------------------


@pytest.fixture(scope="session")
def sysrepo_env(tmp_path_factory: pytest.TempPathFactory) -> dict[str, str]:
    """
    A private sysrepo repository and shared-memory namespace.

    sysrepo creates the repository and installs its internal modules on the
    first connection, so an empty writable directory is enough.
    """
    if shutil.which("sysrepoctl") is None:
        pytest.skip("sysrepoctl not found: build the container image first")

    root = tmp_path_factory.mktemp("sysrepo")
    repo = root / "repository"
    repo.mkdir()

    env = dict(os.environ)
    env["SYSREPO_REPOSITORY_PATH"] = str(repo)
    env["SYSREPO_SHM_PREFIX"] = f"sr_mcp_test_{os.getpid()}"
    env.setdefault("LD_LIBRARY_PATH", "/usr/local/lib")

    return env


@dataclass
class ModuleInstall:
    """Outcome of installing one YANG module, kept so a test can assert on it."""

    name: str
    path: Path
    available: bool          # the .yang file exists
    installed: bool          # sysrepoctl accepted it
    output: str


def _install_module(env: dict[str, str], name: str, path: Path,
                    search_dirs: list[Path]) -> ModuleInstall:
    if not path.is_file():
        return ModuleInstall(name, path, False, False, f"{path} does not exist")

    command = ["sysrepoctl", "--install", str(path)]
    for directory in search_dirs:
        command += ["--search-dirs", str(directory)]

    proc = subprocess.run(command, env=env, capture_output=True, text=True)
    output = f"$ {' '.join(command)}\n{proc.stdout}\n{proc.stderr}"

    return ModuleInstall(name, path, True, proc.returncode == 0, output)


@pytest.fixture(scope="session")
def installed_modules(sysrepo_env: dict[str, str]) -> dict[str, ModuleInstall]:
    """
    Install every YANG module the suite needs, once.

    Failures are recorded rather than raised: the project's own module failing
    to install is a test result worth reporting precisely, and the oven module
    being absent only means extern/ was never downloaded.
    """
    results = {
        "sysrepo-mcp": _install_module(
            sysrepo_env,
            "sysrepo-mcp",
            PROJECT_ROOT / "yang" / "sysrepo-mcp.yang",
            [PROJECT_ROOT / "yang"],
        ),
        "oven": _install_module(
            sysrepo_env,
            "oven",
            OVEN_YANG,
            [OVEN_YANG.parent],
        ),
    }

    return results


@pytest.fixture(scope="session")
def oven_module(
    sysrepo_env: dict[str, str],
    installed_modules: dict[str, ModuleInstall],
) -> dict[str, str]:
    """The environment, with oven.yang guaranteed installed."""
    result = installed_modules["oven"]

    if not result.available:
        pytest.skip(
            f"{OVEN_YANG} not found: run `make -C docker extern` to download "
            "the sysrepo sources"
        )
    if not result.installed:
        pytest.skip(f"sysrepoctl could not install oven.yang:\n{result.output}")

    return sysrepo_env


@pytest.fixture(scope="session")
def mcp_module(
    mcp: "McpClient",
    installed_modules: dict[str, ModuleInstall],
) -> "McpClient":
    """A client, with the project's own YANG module guaranteed installed."""
    result = installed_modules["sysrepo-mcp"]

    if not result.installed:
        pytest.skip(
            "yang/sysrepo-mcp.yang is not installed; see "
            "test_project_module_installs for the reason"
        )

    return mcp


# ---------------------------------------------------------------------------
# Fixtures: the server behind lighttpd
# ---------------------------------------------------------------------------


def _server_binary() -> Path:
    override = os.environ.get("SYSREPO_MCP_BIN")

    if override:
        return Path(override).resolve()

    return PROJECT_ROOT / "build" / "src" / "sysrepo-mcp"


LIGHTTPD_CONF = """\
server.modules = ( "mod_fastcgi" )

server.document-root = "{docroot}"
server.bind          = "{host}"
server.port          = {port}
server.errorlog      = "{errorlog}"
server.pid-file      = "{pidfile}"

fastcgi.debug=65535
debug.log-request-header = "enable"       # Log les entêtes des requêtes reçues
debug.log-response-header = "enable"      # Log les entêtes des réponses envoyées
debug.log-request-handling = "enable"     # Log le cheminement interne de la requête
debug.log-file-not-found = "enable"       # Log l'origine des erreurs 404
debug.log-condition-handling = "enable"   # Log l'évaluation des conditions (vhosts, etc.)

# 0 means no limit: the oversized-body rejection under test belongs to
# sysrepo-mcp, and lighttpd must not answer 413 in its place.
server.max-request-size = 0

fastcgi.server = (
    "{endpoint}" => (
        "sysrepo-mcp" => (
            "socket"          => "{socket}",
            "bin-path"        => "{binary}",
            "check-local"     => "disable",
            "max-procs"       => 1,
            "bin-environment" => (
                "SYSREPO_REPOSITORY_PATH" => "{repository}",
                "SYSREPO_SHM_PREFIX"      => "{shm_prefix}",
                "LD_LIBRARY_PATH"         => "{ld_library_path}"
            )
        )
    )
)
"""


@pytest.fixture(scope="session")
def mcp(
    sysrepo_env: dict[str, str],
    installed_modules: dict[str, ModuleInstall],
    tmp_path_factory: pytest.TempPathFactory,
) -> McpClient:
    """
    Start lighttpd in front of sysrepo-mcp and yield a client.

    lighttpd spawns the server itself through bin-path, which is the
    deployment shape the documentation recommends, and the one where the
    listening socket arrives on descriptor 0.

    It depends on installed_modules rather than on oven_module so that the
    transport and protocol tests still run when extern/ was never downloaded.
    """
    binary = _server_binary()

    if not binary.is_file():
        pytest.skip(f"{binary} not found: build it with scripts/build-docker.sh")
    if not os.access(binary, os.X_OK):
        pytest.skip(f"{binary} is not executable")
    if shutil.which("lighttpd") is None:
        pytest.skip("lighttpd not found: build the container image first")

    root = tmp_path_factory.mktemp("lighttpd")
    docroot = root / "www"
    docroot.mkdir()

    conf = root / "lighttpd.conf"
    errorlog = root / "error.log"
    conf.write_text(
        LIGHTTPD_CONF.format(
            docroot=docroot,
            host=TEST_HOST,
            port=TEST_PORT,
            errorlog=errorlog,
            pidfile=root / "lighttpd.pid",
            endpoint=MCP_ENDPOINT,
            socket=root / "mcp.sock",
            binary=binary,
            repository=sysrepo_env["SYSREPO_REPOSITORY_PATH"],
            shm_prefix=sysrepo_env["SYSREPO_SHM_PREFIX"],
            ld_library_path=sysrepo_env.get("LD_LIBRARY_PATH", "/usr/local/lib"),
        )
    )

    proc = subprocess.Popen(
        ["lighttpd", "-D", "-f", str(conf)],
        env=sysrepo_env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )

    if not _wait_for_port(TEST_HOST, TEST_PORT, STARTUP_TIMEOUT):
        proc.terminate()
        output = proc.communicate(timeout=5)[0].decode(errors="replace")
        pytest.skip(
            f"lighttpd did not listen on {TEST_HOST}:{TEST_PORT}.\n"
            f"lighttpd output:\n{output}\n"
            f"error log:\n{_tail(errorlog)}\n"
            "If the port is the problem, set SYSREPO_MCP_TEST_PORT."
        )

    client = McpClient(TEST_HOST, TEST_PORT)

    # A first request proves the FastCGI process really started: lighttpd
    # spawns it lazily, so binding the port alone proves nothing.
    try:
        probe = client.call("get_status", {})
    except OSError as exc:  # pragma: no cover - environment failure
        _, output = proc.communicate(timeout=5)
        print(f"\n=== sysrepo-mcp server output ===\n{output.decode(errors='replace')}\n{'='*35}", end="")
        pytest.skip(
            f"cannot reach the server: {exc}\n{_tail(errorlog)}"
        )

    if probe.status != 200:
        _, output = proc.communicate(timeout=5)
        print(f"\n=== sysrepo-mcp server output ===\n{output.decode(errors='replace')}\n{'='*35}", end="")
        proc.terminate()
        pytest.fail(
            f"the server answered HTTP {probe.status} to get_status.\n"
            f"body: {probe.body!r}\n"
            f"lighttpd error log:\n{_tail(errorlog)}"
        )

    # Debug: drain and print everything the server has written so far.
    if proc.stdout is not None:
        client._print_pipe(proc.stdout, "server stdout")
    if proc.stderr is not None:
        client._print_pipe(proc.stderr, "server stderr")

    yield client

    # When the fixture tears down, stop the server and print all output.
    proc.send_signal(signal.SIGTERM)
    try:
        output, _ = proc.communicate(timeout=10)
    except subprocess.TimeoutExpired:  # pragma: no cover - environment failure
        proc.kill()
        output, _ = proc.communicate(timeout=5)
    if output:
        print(f"\n=== sysrepo-mcp server output (final) ===\n{output.decode(errors='replace')}\n{'='*38}", end="")


@pytest.fixture(scope="session")
def mcp_oven(mcp: McpClient, oven_module: dict[str, str]) -> McpClient:
    """
    A client, with oven.yang guaranteed installed but no plugin running.

    Schema introspection and configuration writes need the module, not a
    subscriber; only operational reads and RPCs need oven_plugin.
    """
    return mcp


@pytest.fixture
def session(mcp: McpClient) -> McpClient:
    """
    A client bound to its own MCP session, torn down afterwards.

    Function-scoped on purpose: a session carries notification subscriptions
    and a queue, and leaking either into the next test would make failures
    depend on execution order.
    """
    client = mcp.open_session()

    yield client

    client.close_session()


@pytest.fixture
def oven_session(mcp_oven: McpClient) -> McpClient:
    """A session on a server that has the oven module installed."""
    client = mcp_oven.open_session()

    yield client

    client.close_session()


# ---------------------------------------------------------------------------
# Fixtures: the upstream oven plugin
# ---------------------------------------------------------------------------

OVEN_RUNNER_SRC = r"""
/*
 * Standalone runner for the upstream sysrepo oven plugin.
 *
 * extern/sysrepo/examples/plugin/oven.c is written for sysrepo-plugind, which
 * dlopen()s it from a compile-time plugin directory that an unprivileged test
 * run cannot write to. Linking its init and cleanup callbacks into a small
 * process of our own runs exactly the same plugin code with no installation
 * step, which is why the tests exercise the real plugin rather than a stand-in.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sysrepo.h>

int sr_plugin_init_cb(sr_session_ctx_t *session, void **private_data);
void sr_plugin_cleanup_cb(sr_session_ctx_t *session, void *private_data);

static volatile sig_atomic_t stop;

static void
on_signal(int signum)
{
    (void)signum;
    stop = 1;
}

int
main(void)
{
    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *sess = NULL;
    void *pdata = NULL;
    int rc;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    rc = sr_connect(SR_CONN_DEFAULT, &conn);
    if (rc != SR_ERR_OK) {
        fprintf(stderr, "sr_connect: %s\n", sr_strerror(rc));
        return EXIT_FAILURE;
    }

    rc = sr_session_start(conn, SR_DS_RUNNING, &sess);
    if (rc != SR_ERR_OK) {
        fprintf(stderr, "sr_session_start: %s\n", sr_strerror(rc));
        sr_disconnect(conn);
        return EXIT_FAILURE;
    }

    rc = sr_plugin_init_cb(sess, &pdata);
    if (rc != SR_ERR_OK) {
        fprintf(stderr, "sr_plugin_init_cb: %s\n", sr_strerror(rc));
        sr_session_stop(sess);
        sr_disconnect(conn);
        return EXIT_FAILURE;
    }

    printf("oven plugin ready\n");
    fflush(stdout);

    while (!stop)
        pause();

    sr_plugin_cleanup_cb(sess, pdata);
    sr_session_stop(sess);
    sr_disconnect(conn);

    return EXIT_SUCCESS;
}
"""


def _oven_state(client: McpClient) -> dict | None:
    """Read /oven:oven-state, or None when nothing answers."""
    try:
        payload = client.tool("sr_get_operational", {"xpath": "/oven:oven-state"})
    except AssertionError:
        return None

    return payload.get("data", {}).get("oven:oven-state")


@pytest.fixture(scope="session")
def oven_plugin(
    mcp: McpClient,
    oven_module: dict[str, str],
    tmp_path_factory: pytest.TempPathFactory,
) -> McpClient:
    """
    Compile and run the upstream oven plugin, then wait until it answers.

    Without it there is no subscriber for /oven:oven-state nor for the oven
    RPCs, so those tests would be testing sysrepo's "no subscriber" path
    instead of the plugin.
    """
    if not OVEN_PLUGIN_SRC.is_file():
        pytest.skip(f"{OVEN_PLUGIN_SRC} not found: run `make -C docker extern`")

    compiler = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")
    if compiler is None:
        pytest.skip("no C compiler found, cannot build the oven plugin runner")

    root = tmp_path_factory.mktemp("oven")
    source = root / "oven_runner.c"
    source.write_text(OVEN_RUNNER_SRC)
    binary = root / "oven-runner"

    pkg = subprocess.run(
        ["pkg-config", "--cflags", "--libs", "sysrepo", "libyang"],
        capture_output=True,
        text=True,
    )
    pkg_flags = pkg.stdout.split() if pkg.returncode == 0 else ["-lsysrepo", "-lyang"]

    build = subprocess.run(
        [
            compiler,
            "-O0",
            "-g",
            "-o",
            str(binary),
            str(source),
            str(OVEN_PLUGIN_SRC),
            *pkg_flags,
            "-lpthread",
        ],
        capture_output=True,
        text=True,
    )

    if build.returncode != 0:
        pytest.skip(
            "cannot build the oven plugin runner:\n"
            f"{build.stdout}\n{build.stderr}"
        )

    log = root / "oven.log"
    handle = log.open("wb")
    proc = subprocess.Popen(
        [str(binary)],
        env=oven_module,
        stdout=handle,
        stderr=subprocess.STDOUT,
    )

    # Wait until the operational callback answers, rather than sleeping a
    # fixed amount: subscription setup is not instantaneous and the delay is
    # not predictable.
    deadline = time.monotonic() + STARTUP_TIMEOUT
    ready = False

    while time.monotonic() < deadline:
        if proc.poll() is not None:
            handle.close()
            pytest.skip(f"the oven plugin runner exited:\n{_tail(log)}")
        if _oven_state(mcp) is not None:
            ready = True
            break
        time.sleep(POLL_INTERVAL)

    if not ready:
        proc.terminate()
        proc.wait(timeout=5)
        handle.close()
        pytest.skip(
            "the oven plugin never provided /oven:oven-state:\n" f"{_tail(log)}"
        )

    yield mcp

    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:  # pragma: no cover - environment failure
        proc.kill()
        proc.wait(timeout=5)
    handle.close()


@pytest.fixture
def oven_off(oven_plugin: McpClient) -> McpClient:
    """
    Put the oven back to a known state before and after a test.

    The oven plugin keeps state across requests: the configuration lives in
    the datastore, but whether food is inside lives in the plugin process. A
    test that turns the oven on or inserts food must not leak that into the
    next one, so both are reset here.
    """
    client = oven_plugin

    def reset() -> None:
        client.edit_config({"oven:oven": {"turned-on": False, "temperature": 0}})

        # remove-food fails when the oven is already empty, which is the
        # common case; the outcome is irrelevant, only the end state matters.
        client.call("sr_execute_rpc", {"xpath": "/oven:remove-food"})

    reset()
    yield client
    reset()


# ---------------------------------------------------------------------------
# Polling helper, exported to the test modules
# ---------------------------------------------------------------------------


def wait_for(predicate, timeout: float = 10.0, interval: float = POLL_INTERVAL):
    """
    Poll predicate() until it returns something truthy.

    The oven plugin reacts to a configuration change asynchronously, so an
    assertion made immediately after an edit is a race. Returns the last value
    seen, so the caller can assert on it and get a useful failure message.
    """
    deadline = time.monotonic() + timeout
    value = None

    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(interval)

    return value
