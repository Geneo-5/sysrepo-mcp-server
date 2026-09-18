# docker/ — Build environment (Docker)

This directory contains a self-contained **build environment** image for
sysrepo-mcp, meant for **CI and agent-driven builds**. All dependencies are
pre-installed in the image. The project is compiled at runtime on a
bind-mounted working directory, so the sources stay in the host checkout.

The image (based on `debian:trixie-slim`) installs every build & runtime
dependency and compiles the external libraries vendored in `extern/` under
`/usr/local`. The project itself is **not** baked into the image: it is
compiled at run time on a bind-mounted working directory, so the sources
stay in the host checkout.

## Prerequisites

- [Docker](https://www.docker.com/) with BuildKit (default on recent
  versions; check with `docker buildx version`).

## Build the image

From the repository root:

```sh
# Download extern/ sources, then build the image (with Docker cache)
make -C docker build

# Same, but without cache
make -C docker build-nc
```

Both targets first take care of `extern/` (see below), then run
`docker build -t sysrepo-mcp:latest -f docker/Dockerfile .` from the
repository root.

The `extern/` sources are downloaded and extracted from their versioned
upstream URLs:

| Library | Version | Purpose |
|---------|---------|---------|
| ebuild  | master  | Build system |
| libyang | 5.8.6   | YANG schema parsing |
| sysrepo | 5.1.0   | YANG datastore API |
| stroll  | master  | Data structures |
| utils   | master  | eTux utilities |
| elog    | master  | Logging |
| fcgi2   | 2.4.7   | FastCGI transport (required) |

json-c does not come from `extern/`: it is the Debian package
`libjson-c-dev`, installed in the image.

> **Note** : The `extern/` directory contains source references for agents.
> These libraries are compiled and installed in the Docker image at `/usr/local`.

> `extern/` is intentionally excluded from the repository (`.gitignore`).
> It is a download destination, never a source tree — and never modified in
> place: the Dockerfile bind-mounts each source read-only and builds in
> throwaway copies.

## Run the environment

Open an interactive shell inside the build image (the current directory is
mounted at `/home/builder/project`):

```sh
make -C docker run
```

Inside the container, all dependencies are available under `/usr/local`:

```sh
ldconfig
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
make            # build the project
```

## Test

```sh
# Build the image if needed, then run the suite in the container
make -C docker test
```

Or directly, which is the same thing with arguments:

```sh
scripts/test.sh                                   # everything
scripts/test.sh -k oven                           # a pytest filter
scripts/test.sh tests/test_oven.py                # one file
```

The suite drives the server the way a client does: HTTP to lighttpd on port
80, forwarded over FastCGI. It starts lighttpd and the upstream oven plugin
itself, in a private sysrepo repository under the pytest temporary directory,
so it leaves nothing behind and never touches the system repository.

If port 80 is unavailable, set `SYSREPO_MCP_TEST_PORT`.

> Binding port 80 as a non-root user works because Docker sets
> `net.ipv4.ip_unprivileged_port_start=0` inside containers. On a host where
> that is not true, use another port.

## Clean

```sh
# Remove the downloaded extern/ sources (they will be re-downloaded on next
# build)
make -C docker extern-clean
```

## Verification from scratch

The canonical "does the whole environment work?" check:

```sh
make -C docker extern-clean
make -C docker build-nc
```

## Files

- `Dockerfile` — build environment image (all dependencies pre-installed).
- `Makefile` — `build` / `build-nc` / `run` / `test` / `extern` /
  `extern-clean` helpers.

## Extern Directory

The `extern/` directory contains source code for all external dependencies:
- Used as **build sources** for the Docker image (compiled and installed to `/usr/local`)
- Used as **reference material** for IA agents (read-only source code)

> **Important** : `extern/` is excluded from Git (`.gitignore`).
> Sources are automatically downloaded via `make -C docker extern` and must
> never be modified manually.
