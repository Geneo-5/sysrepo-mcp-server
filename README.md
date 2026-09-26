# sysrepo-mcp

An MCP (Model Context Protocol) server, written in C, that exposes the
[sysrepo](https://github.com/sysrepo/sysrepo) YANG datastore to an AI agent:
read and modify configuration, read operational state, invoke RPCs and
actions, and explore YANG schemas.

> **Project status.** The server is functional: FastCGI transport, the MCP
> lifecycle, sessions (`Mcp-Session-Id`), configuration tools, RPC, actions,
> notifications, module management and schema introspection. Authentication
> (API keys from the libconfig file, plaintext at rest and in process memory)
> and NACM are enforced on every operation, including RPCs and actions.
> Constant-time API-key comparison and fail-closed handling of invalid
> configuration remain open under P0 in `sphinx/todo.rst`. Logging uses elog
> with configurable syslog, file and console back ends. The shared session
> store is still absent (`max-procs` must stay
> 1). The detailed roadmap, which is authoritative on the real state of the
> code, is in
> [`sphinx/todo.rst`](sphinx/todo.rst).

> **Access control is only as good as the deployment config.** With no API
> keys configured, every request is served with the rights of the system
> user running the server. Configure `auth.api_keys[]` in the libconfig file
> and NACM rules for each identity before exposing this to an agent you do
> not fully trust.

> **`max-procs` must be 1.** A session, its notification subscriptions and
> its queue live in the FastCGI process that created it. With more than one
> process, consecutive requests from the same agent can land in different
> processes and the session is not found.

## Features

- **Configuration**: read, modify and delete configuration through the
  sysrepo API (`sr_get_config`, `sr_edit_config`, `sr_delete_config`, `sr_copy_config`).
- **Monitoring**: read operational state (`sr_get_operational`).
- **Operations**: invoke YANG RPCs and actions.
- **Notifications**: subscribe a session to a module's notifications and
  collect, on demand, everything that arrived since the last call
  (`sr_notif_subscribe`, `sr_notif_poll`). By polling only — this transport
  cannot push.
- **Modules**: list, install and uninstall YANG modules.
- **Introspection**: explore the compiled schema (`get_schema`) recursively,
  including paths, constraints, defaults, RPC inputs and descriptions, so an
  agent can build valid requests without reading the YANG source.
- **Security**: an API key per agent, bound to a NACM user, with every
  operation — datastore reads/writes, RPCs and actions — checked against
  NACM, plus a module allow-list, an operation filter and write protection
  in front of sysrepo. Logging of every configuration change with the
  identity that made it.

## Architecture

```
┌─────────────┐  HTTP  ┌───────────────┐ FastCGI ┌───────────────┐
│  AI agent   │◄──────►│ Reverse proxy │◄───────►│  sysrepo-mcp  │
│ (MCP client)│  TLS   │ (lighttpd,    │         │ (this project)│
└─────────────┘        │  nginx)       │         └───────┬───────┘
                       └───────────────┘                 │ linked
                                                         ▼
                                                 ┌───────────────┐
                                                 │  libsysrepo   │
                                                 │  + libyang    │
                                                 └───────┬───────┘
                                                         ▼
                                            ┌──────────────────────┐
                                            │ /etc/sysrepo (YANG,  │
                                            │ startup) + /dev/shm  │
                                            │ (running, locks)     │
                                            └──────────────────────┘
```

Two decisions shape everything else:

- **FastCGI transport only.** The server never speaks HTTP itself. It can be
  spawned by lighttpd or create a Unix/TCP FastCGI listener; a reverse proxy
  terminates HTTP and TLS. No public HTTP listener, no SSE (see *Limitations*).
- **sysrepo as a library.** `libsysrepo` is linked into the binary and
  called directly. There is no daemon: sysrepo has had none since version 2.

Full detail in [`sphinx/architecture.rst`](sphinx/architecture.rst).

## Dependencies

| Library | Version | Role |
|---|---|---|
| **eBuild** | master | Build system (Kconfig-based Makefile framework, not linked) |
| **libyang** | 5.8.6 | YANG engine |
| **sysrepo** | 5.1.0 | YANG datastore API |
| **fcgi2** | 2.4.7 | FastCGI transport (`libfcgi`) |
| **stroll** | master | Data structures |
| **utils** | master | eTux utilities |
| **elog** | master | Logging |
| **json-c** | `libjson-c-dev` | JSON-RPC |

All of these must already be built and installed on the machine that
builds sysrepo-mcp — for example under `/usr/local`, discoverable through
`PKG_CONFIG_PATH` — except `json-c`, which is a regular distribution
package. Nothing in this repository downloads or builds them for a normal
build.

> `extern/` does **not** belong to this list. It is a read-only
> reference/build-cache directory that is part of the agent- and
> CI-oriented development environment described under *Reproducible build
> & test environment* below — not something a regular build populates or
> requires.

## Building

Once the dependencies above are installed and discoverable via
pkg-config:

```sh
make config                # interactive eBuild/Kconfig menu
# or: make defconfig       # non-interactive, built-in defaults
make                        # compiles build/sysrepo-mcp
make install PREFIX=/usr/local
```

`config.in` documents the available Kconfig options and their defaults; all
of them are compile-time only, runtime configuration lives in a libconfig
file (see *Usage* below).

### Reproducible build & test environment (Docker)

`docker/` and `extern/` together form a disposable, agent- and CI-oriented
environment: `extern/` holds the pinned dependency sources, fetched with
`make -C docker extern` (plain `curl`/`tar` targets, independent of Docker
itself, and never edited in place), and the Docker image builds and
installs all of them under `/usr/local` so CI or an AI agent gets a working
toolchain without touching the host. It is not a requirement to build or
deploy the server — see *Dependencies* above for the normal path. Full
workflow in [docker/README.md](docker/README.md):

```sh
make -C docker build     # build the image (downloads extern/ first)
make -C docker run       # interactive shell, every dependency in place
make -C docker test      # run the pytest suite against it
```

`scripts/build-docker.sh` wraps the same image from the host, for a
one-shot binary, documentation or test build:

```sh
scripts/build-docker.sh              # binary
scripts/build-docker.sh --doc        # binary + documentation
scripts/build-docker.sh --test       # binary + test suite
scripts/build-docker.sh --force      # re-download extern/ and rebuild the image
```

## Usage

```sh
sysrepo-mcp [--help] [--version] [-f <file>] [-l <severity>]
```

The runtime configuration defaults to `/etc/sysrepo-mcp/sysrepo-mcp.conf`.
`server.transport.mode` selects `proxy` (default), `unix`, or `tcp`. In
`proxy` mode, lighttpd starts the FastCGI process:

```
server.modules += ( "mod_fastcgi" )

fastcgi.server = (
    "/mcp" => (
        "sysrepo-mcp" => (
            "socket"      => "/var/run/sysrepo-mcp.sock",
            "bin-path"    => "/usr/local/bin/sysrepo-mcp",
            "check-local" => "disable",
            "max-procs"   => 1
        )
    )
)
```

For standalone operation, select `unix` or `tcp`; the program creates the
FastCGI listener itself. The sample config uses a Unix socket at
`/run/sysrepo-mcp/mcp.sock`; the TCP mode defaults to `127.0.0.1:8080`.
These sockets carry FastCGI, not HTTP. Place a FastCGI-capable reverse proxy
in front for HTTP clients. Lighttpd and standalone systemd examples are in
[`contrib/`](contrib/).

### Example session

```sh
# 1. Open a session: the identifier comes back in the header
curl -i -X POST http://localhost/mcp \
     -H 'Accept: application/json, text/event-stream' \
     -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":1,"method":"initialize",
          "params":{"protocolVersion":"2025-11-25"}}'

# 2. Subscribe to notifications, repeating the header
curl -X POST http://localhost/mcp \
     -H 'Accept: application/json, text/event-stream' \
     -H 'Content-Type: application/json' \
     -H 'Mcp-Session-Id: <identifier>' \
     -d '{"jsonrpc":"2.0","id":2,"method":"tools/call",
          "params":{"name":"sr_notif_subscribe",
                    "arguments":{"module":"oven"}}}'

# 3. Collect what arrived since the last call
curl -X POST http://localhost/mcp \
     -H 'Accept: application/json, text/event-stream' \
     -H 'Content-Type: application/json' \
     -H 'Mcp-Session-Id: <identifier>' \
     -d '{"jsonrpc":"2.0","id":3,"method":"tools/call",
          "params":{"name":"sr_notif_poll","arguments":{}}}'

# 4. Close the session
curl -X DELETE http://localhost/mcp -H 'Mcp-Session-Id: <identifier>'
```

## Documentation

```sh
scripts/build-docker.sh --doc        # HTML, PDF, info, man
```

| Format | Output |
|---|---|
| HTML | `build/doc/html/index.html` |
| PDF | `build/doc/pdf/sysrepo-mcp.pdf` |
| Info | `build/doc/info/sysrepo-mcp.info` |
| Man | `build/doc/man/` |

The sources live in `sphinx/`: installation, architecture, API reference,
license, roadmap.

## Project layout

```
├── docker/                 # Agent/CI dev environment (build image, not required)
│   ├── Dockerfile          # build image (every dependency pre-installed)
│   ├── Makefile             # build / build-nc / run / test / extern targets
│   └── lighttpd.conf        # FastCGI proxy used by the test suite
├── extern/                 # Pinned dependency sources for docker/ (out of Git)
├── include/sysrepo/mcp/    # Public headers
├── scripts/                # build-docker.sh, test.sh
├── sphinx/                 # Documentation (RST + Doxyfile)
├── src/                    # Server source code
├── tests/                  # pytest suite
├── config.in                # Kconfig options (build time)
├── Makefile                 # eBuild entry point
└── ebuild.mk                 # Binary declaration
```

## Limitations

- **No SSE.** The server answers a POST with a single JSON object, which
  the MCP Streamable HTTP binding explicitly allows. This is a scope
  choice, not a technical impossibility: FastCGI can stream, but proxy
  buffering and the `max-procs` process model are a poor fit for it.
  Consequence: **sysrepo notifications cannot be pushed to the agent.**
  They are not lost, though — the server subscribes on the agent's behalf,
  queues them, and hands them all over when it calls `sr_notif_poll`. MCP
  sampling and elicitation are out of scope for the same reason.
- **`max-procs = 1`.** Sessions are local to the process; see the roadmap,
  milestone 4.
- **No direct HTTP listener.** HTTP, TLS and rate limiting stay the
  responsibility of the reverse proxy; standalone sockets carry FastCGI.
- **RPC/action authorization.** See the project status above and
  `sphinx/todo.rst`, the completed security work and remaining P0 items.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) and
[COPYING.txt](COPYING.txt).

The linked libraries have their own licenses: sysrepo and libyang are
BSD-3-Clause, json-c is MIT, fcgi2 is under the FastCGI license. Detail in
`sphinx/license.rst`.
