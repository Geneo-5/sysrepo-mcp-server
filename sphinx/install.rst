.. Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

Integration Guide
=================

This chapter describes how to build and install sysrepo-mcp.

.. warning::

   The build system and the container environment are functional. The server
   itself dispatches MCP tools over a fully implemented lifecycle, with
   authentication (API keys) and access control (NACM, module filter, write
   protection).  See the :doc:`todo` appendix for remaining work.

Prerequisites
-------------

Everything is provided by the Docker build image; nothing has to be installed
on the host beyond:

- Docker with BuildKit (default on recent versions, check with
  ``docker buildx version``)
- GNU make, to drive ``docker/Makefile`` from the host

Inside the image, the build needs:

- GCC 8+ (or Clang), ``pkg-config``, ``kconfig-frontends``
- eBuild, installed under ``/usr/share/ebuild``
- ``libyang``, ``sysrepo``, ``fcgi2``, ``stroll``, ``utils``, ``elog``, built
  from ``extern/`` and installed under ``/usr/local``
- ``json-c``, from the Debian package ``libjson-c-dev``
- Sphinx, ``sphinx-rtd-theme``, Breathe, Doxygen and TeX Live, for the
  documentation
- Python 3 and pytest, for the test suite

.. note::

   ``extern/`` is a *download destination*, not a source tree. It is excluded
   from Git (see ``.gitignore``), populated by ``make -C docker extern``, and
   must never be modified in place. The Dockerfile bind-mounts each library
   read-only and builds it in a throwaway copy.

Installation Guide
------------------

Using Docker (the only supported method)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. **Build the image**. This downloads the ``extern/`` sources, then compiles
   and installs every dependency into the image:

   .. code-block:: bash

      make -C docker build

   Or, without the Docker layer cache:

   .. code-block:: bash

      make -C docker build-nc

2. **Open the build environment**. The working copy is bind-mounted, so the
   sources stay in the host checkout:

   .. code-block:: bash

      make -C docker run

3. **Configure the build** (optional). Options are declared in ``config.in``
   and are all *build-time* options:

   .. code-block:: bash

      make config   # interactive Kconfig interface (menuconfig)

   or, to accept every default without any interaction:

   .. code-block:: bash

      make defconfig

4. **Build**:

   .. code-block:: bash

      make

   The binary lands in ``build/sysrepo-mcp``.

5. **Install** (optional):

   .. code-block:: bash

      make install PREFIX=/usr/local

6. **Clean**:

   .. code-block:: bash

      make clean

Everything above can also be driven from the host with a single script, which
wraps the same targets in a ``docker run``:

.. code-block:: bash

   scripts/build-docker.sh              # build the binary
   scripts/build-docker.sh --doc        # build the binary and all the docs
   scripts/build-docker.sh --doc-html   # HTML documentation only
   scripts/build-docker.sh --test       # build, then run the test suite
   scripts/build-docker.sh --force      # re-download extern/ and rebuild the image

.. note::

   Rebuilding the image is slow and rarely needed: the ``extern/`` libraries
   change only when their pinned versions in ``docker/Makefile`` change. Use
   ``--force`` only after bumping a dependency, editing the ``Dockerfile``, or
   changing a system package.

Building the documentation
~~~~~~~~~~~~~~~~~~~~~~~~~~

The eBuild documentation targets produce, from ``sphinx/``:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Target
     - Output
   * - ``make doc``
     - Everything below
   * - ``make html``
     - ``build/doc/html/index.html``
   * - ``make pdf``
     - ``build/doc/pdf/sysrepo-mcp.pdf``
   * - ``make info``
     - ``build/doc/info/sysrepo-mcp.info``
   * - ``make man``
     - ``build/doc/man/``

Doxygen extracts the public headers of ``include/`` into
``sphinx/_doxygen/xml``; ``sphinx/conf.py`` enables Breathe only when that
directory exists, so ``sphinx-build`` also works without running Doxygen first.

Configuration Options
---------------------

All options below are declared in ``config.in`` (Kconfig format) and are fixed
at compile time. Runtime configuration, namely the API keys, lives in the
``sysrepo-mcp`` YANG module (see :doc:`architecture`).

In the generated header, every symbol is prefixed with ``CONFIG_``; for
instance ``server.log.level`` is used from C as
``CONFIG_server.log.level``.

Sessions and notifications
~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 15 45

   * - Option
     - Default
     - Description
   * - ``SYSREPO_MCP_SERVER_MAX_SESSIONS``
     - ``64``
     - Concurrent MCP sessions (1-1024). Past it, ``initialize`` returns
       HTTP 503.
   * - ``SYSREPO_MCP_SERVER_SESSION_TTL``
     - ``1800``
     - Seconds of inactivity before a session, its subscriptions and its
       queued notifications are destroyed (60-86400).
   * - ``SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE``
     - ``256``
     - Notifications buffered per session between two polls (8-65536).

.. warning::

   These are build-time because the session store is a fixed array in the
   process. That also means the FastCGI configuration **must** use
   ``max-procs = 1``: with more, a session created by one worker is invisible
   to the next request. See :doc:`architecture`.

FastCGI transport
~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 15 45

   * - Option
     - Default
     - Description
   * - ``SYSREPO_MCP_SERVER_TRANSPORT_UNIX``
     - ``y``
     - Listen on a Unix socket. Mutually exclusive with the TCP choice.
   * - ``SYSREPO_MCP_SERVER_TRANSPORT_TCP``
     - ``n``
     - Listen on a TCP socket instead.
   * - ``SYSREPO_MCP_SERVER_UNIX_SOCKET_PATH``
     - ``/var/run/sysrepo-mcp.sock``
     - Socket path, when the Unix transport is selected.
   * - ``SYSREPO_MCP_SERVER_TCP_HOST``
     - ``127.0.0.1``
     - Bind address, when the TCP transport is selected.
   * - ``SYSREPO_MCP_SERVER_TCP_PORT``
     - ``8080``
     - Bind port (1-65535), when the TCP transport is selected.

.. note::

   These options describe where the server listens for *FastCGI* connections
   coming from the reverse proxy. They are not an HTTP listener: HTTP is
   terminated by the proxy.

   When the proxy spawns the server itself (lighttpd ``bin-path``), the socket
   is handed over on descriptor 0 and these options are unused.

Access control
~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 15 45

   * - Option
     - Default
     - Description
   * - ``SYSREPO_MCP_SERVER_ACL_ENABLED``
     - ``y``
     - Master switch for authentication and NACM.
   * - ``SYSREPO_MCP_SERVER_AUTH_BEARER``
     - ``y``
     - Read the credential from ``Authorization: Bearer <key>``.
   * - ``SYSREPO_MCP_SERVER_AUTH_COOKIE``
     - ``n``
     - Read the credential from a cookie instead.
   * - ``SYSREPO_MCP_SERVER_COOKIE_NAME``
     - ``mcp_session``
     - Cookie name, when the cookie credential is selected.
   * - ``SYSREPO_MCP_SERVER_ACL_ENABLE_NACM``
     - ``y``
     - Bind the session to a NACM user (``sr_nacm_set_user()``).
   * - ``SYSREPO_MCP_SERVER_ACL_ENABLE_MODULE_FILTER``
     - ``y``
     - Restrict the reachable YANG modules.
   * - ``SYSREPO_MCP_SERVER_ACL_ENABLE_OPERATION_FILTER``
     - ``n``
     - Distinguish read from write when filtering.
   * - ``SYSREPO_MCP_SERVER_ACL_ENABLE_WRITE_PROTECTION``
     - ``n``
     - Refuse every write on the modules flagged as sensitive.
   * - ``SYSREPO_MCP_SERVER_ACL_ALLOWED_MODULES``
     - ``""``
     - Comma-separated module allow-list; empty means every module.

.. warning::

   None of the access control options are enforced yet. Building with
   ``SYSREPO_MCP_SERVER_ACL_ENABLED=y`` currently grants an agent the same
   rights as the user the server runs as.

sysrepo repository
~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 15 45

   * - Option
     - Default
     - Description
   * - ``SYSREPO_MCP_SERVER_SYSREPO_DATASTORE_DIR``
     - ``/etc/sysrepo``
     - Repository directory used by the linked sysrepo library.

.. note::

   sysrepo resolves its repository path at *its own* compile time
   (``-DREPO_PATH``), and honours the ``SYSREPO_REPOSITORY_PATH`` environment
   variable at runtime. This option only controls what sysrepo-mcp exports in
   that variable before connecting; it cannot move a repository that sysrepo
   was built to use elsewhere.

Logging
~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 15 45

   * - Option
     - Default
     - Description
   * - ``server.log.syslog_enabled``
     - ``true``
     - Log to syslog through elog.
   * - ``server.log.file``
     - ``/var/log/sysrepo-mcp.log``
     - Append-only file log path; can be enabled together with syslog and console.
   * - ``server.log.level``
     - ``6``
     - Syslog severity, 0 (emerg) to 7 (debug).
   * - ``server.log.verbose``
     - ``false``
     - Extra debugging output.
   * - ``server.log.console``
     - ``true``
     - Also log to ``stderr``.

.. warning::

   Under FastCGI, ``stderr`` is captured by the web server and ends up in its
   error log. Console logging is therefore only useful when the process is
   started by the proxy; it is not a substitute for syslog.

Usage
-----

.. code-block:: bash

   sysrepo-mcp [--help] [--version] [-f <file>] [-l <severity>]

``--help`` prints a usage summary and exits, ``--version`` prints the version
and exits. ``--log-level`` accepts elog severities ``emerg``, ``alert``,
``crit``, ``err``, ``warn``, ``notice``, ``info`` or ``debug`` and overrides
the libconfig threshold. With no argument the process expects to be started
as a FastCGI application by a web server and exits with an error otherwise.

See :doc:`architecture` for the reverse proxy configuration.

Dependencies
------------

.. list-table::
   :header-rows: 1
   :widths: 18 14 20 48

   * - Library
     - Version
     - Origin
     - Purpose
   * - **eBuild**
     - master
     - ``extern/``
     - Makefile build framework (build only, not linked)
   * - **libyang**
     - 5.8.6
     - ``extern/``
     - YANG schema and data tree engine
   * - **sysrepo**
     - 5.1.0
     - ``extern/``
     - YANG datastore API
   * - **fcgi2**
     - 2.4.7
     - ``extern/``
     - FastCGI transport (``libfcgi``), mandatory
   * - **stroll**
     - master
     - ``extern/``
     - Data structures (lists, hashes, buffers)
   * - **utils**
     - master
     - ``extern/``
     - eTux utilities (file, network, thread)
   * - **elog**
     - master
     - ``extern/``
     - Logging (syslog and standard I/O; file output uses elog interface)
   * - **json-c**
     - Debian ``libjson-c-dev``
     - system package
     - JSON-RPC parsing and generation

The pinned versions live in ``docker/Makefile``; libyang 5.8.6 and sysrepo
5.1.0 are the pair used together by Netopeer2 2.8.7.

License
-------

sysrepo-mcp is distributed under the GNU Lesser General Public License,
version 3. See the :ref:`license` appendix, which also lists the licenses of
the third-party libraries above.
