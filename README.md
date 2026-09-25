# sysrepo-mcp

An MCP (Model Context Protocol) server, written in C, that exposes the
[sysrepo](https://github.com/sysrepo/sysrepo) YANG datastore to an AI agent:
read and modify configuration, read operational state, invoke RPCs and
actions, and explore YANG schemas.

> **Project status.** The server is functional: FastCGI transport, the MCP
> lifecycle, sessions (`Mcp-Session-Id`), configuration tools, RPC, actions,
> notifications, module management and schema introspection. Authentication
> (API keys) and NACM are wired in, but per-operation authorization is not
> yet enforced on RPCs and actions — see `sphinx/todo.rst`, P0.10. Still
> missing: elog, and a shared session store. The detailed roadmap, which is
> authoritative on the real state of the code, is in
> [`sphinx/todo.rst`](sphinx/todo.rst).

> **Do not expose this version to an untrusted agent** without reading
> `sphinx/todo.rst`, P0.10, first. Datastore reads and writes go through
> sysrepo's NACM once authentication is configured, but RPCs and actions are
> not checked against NACM before being invoked: an authenticated agent can
> currently call any RPC or action of any installed module.

> **`max-procs` must be 1.** A session, its notification subscriptions and
> its queue live in the FastCGI process that created it. With more than one
> process, consecutive requests from the same agent can land in different
> processes and the session is not found.

## Features

- **Configuration**: read, modify and delete configuration through the
  sysrepo API (`sr_get_config`, `sr_edit_config`, `sr_delete_config`).
- **Monitoring**: read operational state (`sr_get_operational`).
- **Operations**: invoke YANG RPCs and actions.
- **Notifications**: subscribe a session to a module's notifications and
  collect, on demand, everything that arrived since the last call
  (`sr_notif_subscribe`, `sr_notif_poll`). By polling only — this transport
  cannot push.
- **Modules**: list, install and uninstall YANG modules.
- **Introspection**: explore a schema (`get_tree`) and document a node
  (`get_help`), so an agent can build valid XPaths without reading the YANG
  source.
- **Security**: an API key per agent, bound to a NACM user, and logging of
  every configuration change. RPC/action-level authorization is still open —
  see the project status above.

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

- **FastCGI transport only.** The server never speaks HTTP itself: a reverse
  proxy terminates HTTP and TLS and forwards to it over FastCGI. No public
  socket, no SSE (see *Limitations*).
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
sysrepo-mcp [--help] [--version] [-f | --config <file>]
```

With no argument, the process waits to be started as a FastCGI application
by a web server, reading its runtime configuration from
`/etc/sysrepo-mcp/sysrepo-mcp.conf` by default. Example lighttpd
configuration:

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

### Example session

```sh
# 1. Open a session: the identifier comes back in the header
curl -i -X POST http://localhost/mcp \
     -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":1,"method":"initialize",
          "params":{"protocolVersion":"2025-06-18"}}'

# 2. Subscribe to notifications, repeating the header
curl -X POST http://localhost/mcp \
     -H 'Content-Type: application/json' \
     -H 'Mcp-Session-Id: <identifier>' \
     -d '{"jsonrpc":"2.0","id":2,"method":"tools/call",
          "params":{"name":"sr_notif_subscribe",
                    "arguments":{"module":"oven"}}}'

# 3. Collect what arrived since the last call
curl -X POST http://localhost/mcp \
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
- **No direct transport.** HTTP, TLS and rate limiting stay the
  responsibility of the reverse proxy.
- **RPC/action authorization.** See the project status above and
  `sphinx/todo.rst`, P0.10.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) and
[COPYING.txt](COPYING.txt).

The linked libraries have their own licenses: sysrepo and libyang are
BSD-3-Clause, json-c is MIT, fcgi2 is under the FastCGI license. Detail in
`sphinx/license.rst`.
