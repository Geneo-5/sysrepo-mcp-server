# ==============================================================================
# sysrepo-mcp-server -- Build environment image
#
# This image is a self-contained build environment for sysrepo-mcp-server.
# It installs every build & runtime dependency of the project and of the
# external libraries vendored under extern/, then lets the caller (see
# scripts/test.sh) compile and test the project at run time.
#
# It deliberately does NOT build the project itself : the working directory is
# bind-mounted into the container at run time and `make test` is invoked there.
#
#   docker build -t sysrepo-mcp-builder:latest -f docker/Dockerfile .
#   docker run -u $(id -u):$(id -g) -v $PWD:$PWD -w $PWD \
#          sysrepo-mcp-builder:latest make test
# ==============================================================================
FROM debian:trixie-slim

LABEL maintainer="Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>"
LABEL org.opencontainers.image.title="sysrepo-mcp-server build environment"

# ----------------------------------------------------------------------------
# 0. Build-time environment variables
# ----------------------------------------------------------------------------
# C build flags, shared by the external libraries compiled in this image.
ENV CFLAGS="-Wall -Wextra -Wshadow -Wformat=2 -Wuninitialized \
	-fstack-protector-strong -D_FORTIFY_SOURCE=2 \
	-O0 -ggdb3"
ENV CXXFLAGS="${CFLAGS}"
ENV LDFLAGS="-Wl,-z,relro,-z,now"

# ----------------------------------------------------------------------------
# 1. System packages
# ----------------------------------------------------------------------------
# These cover the dependencies of every extern/ library and of the project
# itself (json-c for the JSON-RPC layer, python3/pytest for the test suite,
# lighttpd as the background HTTP server, plus debugging tools).
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        # Base / build essentials
        build-essential \
        g++ \
        make \
        gcc \
        gcc-aarch64-linux-gnu \
        libc6-dev \
        autoconf \
        automake \
        libtool \
        m4 \
        pkg-config \
        git \
        kconfig-frontends \
        python3-pip \
        # Download / archive tools (needed by some extern build systems)
        wget \
        curl \
        ca-certificates \
        unzip \
        # CMake (libyang, sysrepo)
        cmake \
        # C runtime libraries required by the extern/ sources
        libev-dev \
        libncurses-dev \
        libxml2-dev \
        libsqlite3-dev \
        uuid-dev \
        libuv1-dev \
        libkmod-dev \
        libpcre2-dev \
        zlib1g-dev \
        libssl-dev \
        libcurl4-openssl-dev \
        libhiredis-dev \
        libnghttp2-dev \
        doxygen \
        # JSON-RPC message parsing (the project)
        libjson-c-dev \
        # Python test runner (the suite uses the standard library only)
        python3 \
        python3-pytest \
        # HTTP server running in the background
        lighttpd \
        # Debugging / inspection
        strace \
        gdb \
        valgrind \
        texlive-latex-base \
        texlive-latex-recommended \
        texlive-fonts-recommended \
        texlive-fonts-extra \
        texlive-latex-extra \
        tex-gyre \
        latexmk \
        texinfo \
        graphviz \
    && rm -rf /var/lib/apt/lists/*

# NOTE: libconfig-dev is deliberately absent. The project has no runtime
# configuration file: build-time options are Kconfig (config.in) and runtime
# options are YANG (yang/sysrepo-mcp.yang). See sphinx/todo.rst.
    # Refresh TeX path database so that kpsewhich finds .sty in /usr/share/texmf
RUN texhash 2>/dev/null || true && \
    updmap-sys --syncwithtrees 2>/dev/null || true
RUN pip install --break-system-packages kconfiglib
RUN pip install --break-system-packages sphinx sphinx-rtd-theme breathe

# ----------------------------------------------------------------------------
# 2. Compile & install the external libraries from extern/
# ----------------------------------------------------------------------------
# The build context is the project root (see .dockerignore) and contains the
# extern/ tree. Each library is compiled and installed under /usr/local so the
# project can find them via the standard paths.
#
#   - libyang  5.8.6   (cmake)
#   - sysrepo  5.1.0   (cmake)
#   - fcgi2    2.4.7   (autotools)
#   - ebuild            (make install)
#   - stroll          (make)
#   - utils             (make)
#
# `--mount=type=bind,readonly` exposes the single extern/<lib> source tree
# needed by a given step, read-only so the sources are never modified in place.
#
# fcgi2 note: the vendored git snapshot ships no pre-generated configure
# script, so it is bootstrapped with autogen.sh (autoreconf).
# --disable-examples drops the examples and @LIBFCGIXX@ is empty when no C++
# compiler works, so only the libfcgi C library, cgi-fcgi and headers are built.

# --- 2.1 ebuild (build system, no C compilation) ----------------------------
RUN --mount=type=bind,source=extern/ebuild,target=/src/ebuild,readonly \
    mkdir -p /build && \
    cp -a /src/ebuild /build/ebuild && \
    cd /build/ebuild && \
    make PREFIX=/usr install && \
    chmod -R +x /usr/share/ebuild/scripts 2>/dev/null || true

# --- 2.2 libyang 5.8.6 (sysrepo's YANG engine) -------------------------------
RUN --mount=type=bind,source=extern/libyang,target=/src/libyang,readonly \
    cmake -S /src/libyang -B /build/libyang \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_TESTING=OFF && \
    cmake --build /build/libyang -j$(nproc) && \
    cmake --install /build/libyang

# --- 2.3 sysrepo 5.1.0 (NETCONF / YANG configuration datastore) --------------
RUN --mount=type=bind,source=extern/sysrepo,target=/src/sysrepo,readonly \
    cmake -S /src/sysrepo -B /build/sysrepo \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_TESTING=OFF \
        -DSR_MSG_INTERNAL=ON && \
    cmake --build /build/sysrepo -j$(nproc) && \
    cmake --install /build/sysrepo && \
    ldconfig && \
    sysrepoctl -i /src/sysrepo/examples/plugin/oven.yang && \
    sysrepo-plugind -P /build/sysrepo/examples/oven.so && \
    rm -rf /dev/shm/*

# --- 2.4 fcgi2 2.4.7 (FastCGI) -----------------------------------------------
# The vendored git snapshot ships no pre-generated configure script, so it is
# bootstrapped (autogen.sh) in a throwaway copy, then configured from an
# out-of-tree build directory. --disable-examples drops the examples and
# @LIBFCGIXX@ is empty when no C++ compiler works, leaving only the libfcgi C
# library, cgi-fcgi and the public headers.
RUN --mount=type=bind,source=extern/fcgi2,target=/src/fcgi2,readonly \
    cp -a /src/fcgi2 /build/fcgi2 && \
    cd /build/fcgi2 && \
    ./autogen.sh && \
    mkdir build && \
    cd build && \
    ../configure --prefix=/usr/local --disable-examples --disable-dependency-tracking && \
    make -j$(nproc) && \
    make install

# libyang, sysrepo and fcgi2 install under /usr/local, the eTux libraries under
# /usr. Both prefixes must stay searchable: overwriting PKG_CONFIG_PATH with
# /usr/lib/pkgconfig alone hides libyang.pc and sysrepo.pc, and `pkg-config
# --libs sysrepo` then silently returns nothing.
ENV PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:/usr/local/share/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig

# --- 2.5 stroll (data structures) --------------------------------------------
RUN --mount=type=bind,source=extern/stroll,target=/src/stroll,readonly \
    cp -a /src/stroll /build/stroll-src && \
    cd /build/stroll-src && \
    make EXTRA_CFLAGS="${CFLAGS}" EXTRA_LDFLAGS="${LDFLAGS}" PREFIX=/usr BUILDDIR=/build/stroll defconfig && \
    make EXTRA_CFLAGS="${CFLAGS}" EXTRA_LDFLAGS="${LDFLAGS}" PREFIX=/usr BUILDDIR=/build/stroll build && \
    make EXTRA_CFLAGS="${CFLAGS}" EXTRA_LDFLAGS="${LDFLAGS}" PREFIX=/usr BUILDDIR=/build/stroll install

# --- 2.6 utils (eTux utilities) ----------------------------------------------
RUN --mount=type=bind,source=extern/utils,target=/src/utils,readonly \
    cp -a /src/utils /build/utils-src && \
    cd /build/utils-src && \
    make EXTRA_CFLAGS="${CFLAGS}" EXTRA_LDFLAGS="${LDFLAGS}" PREFIX=/usr BUILDDIR=/build/utils defconfig && \
    make EXTRA_CFLAGS="${CFLAGS}" EXTRA_LDFLAGS="${LDFLAGS}" PREFIX=/usr BUILDDIR=/build/utils build && \
    make EXTRA_CFLAGS="${CFLAGS}" EXTRA_LDFLAGS="${LDFLAGS}" PREFIX=/usr BUILDDIR=/build/utils install

# --- 2.7 elog (syslog utilities) ----------------------------------------------
RUN --mount=type=bind,source=extern/elog,target=/src/elog,readonly \
    cp -a /src/elog /build/elog-src && \
    cd /build/elog-src && \
    make EXTRA_CFLAGS="${CFLAGS}" EXTRA_LDFLAGS="${LDFLAGS}" PREFIX=/usr BUILDDIR=/build/elog defconfig && \
    make EXTRA_CFLAGS="${CFLAGS}" EXTRA_LDFLAGS="${LDFLAGS}" PREFIX=/usr BUILDDIR=/build/elog build && \
    make EXTRA_CFLAGS="${CFLAGS}" EXTRA_LDFLAGS="${LDFLAGS}" PREFIX=/usr BUILDDIR=/build/elog install

# Refresh the dynamic linker cache for the freshly installed libraries.
RUN ldconfig
RUN rm -rf /build

# ----------------------------------------------------------------------------
# 3. lighttpd (background HTTP server)
# ----------------------------------------------------------------------------
# The default lighttpd config runs as its package user/group; the service is
# simply made available to be started on demand (see CMD below).
VOLUME /var/lib/lighttpd

# ----------------------------------------------------------------------------
# 4. Default behaviour
# ----------------------------------------------------------------------------
# Default entry point runs lighttpd in the background, then execs the requested
# command. Override it with `make test` (see README) to build & test the project.
CMD ["bash", "-lc", "lighttpd -D -f /etc/lighttpd/lighttpd.conf & sleep 1; exec bash"]
