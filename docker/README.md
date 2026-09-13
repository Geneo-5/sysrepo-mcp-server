# docker/ — Build environment (Docker)

This directory contains a self-contained **build environment** image for
sysrepo-mcp-server, meant for **CI and agent-driven builds** — not the usual
development workflow (see the root [README](../README.md) for the standard
`make`-based build, which assumes the dependencies are already installed on
the build host).

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
`docker build -t sysrepo-mcp-server:latest -f docker/Dockerfile .` from the
repository root.

The `extern/` sources are downloaded and extracted from their versioned
upstream URLs:

| Library | Version |
|---------|---------|
| ebuild  | master  |
| libyang | 5.8.6   |
| sysrepo | 5.1.0   |
| stroll  | master  |
| utils   | master  |
| fcgi2   | 2.4.7   |

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
# Build the image, then run `make test` in the container
# (smoke tests the binary + runs tests/ if present)
make -C docker test
```

A filter can be passed to the pytest suite:

```sh
make -C docker test test_config
```

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

- `Dockerfile` — build environment image (packages + extern/ libraries).
- `Makefile` — `build` / `build-nc` / `run` / `test` / `extern` /
  `extern-clean` helpers.
- `config.cfg` — reference [libconfig](http://www.cksystem.com/libconfig/)
  configuration sample for the server.
