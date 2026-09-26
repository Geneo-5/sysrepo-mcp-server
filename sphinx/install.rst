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

Configuration
-------------

Kconfig contains only build-time choices. The runtime settings live in the
libconfig file, ``/etc/sysrepo-mcp/sysrepo-mcp.conf`` by default.

Build-time options
~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 20 40

   * - Option
     - Default
     - Description
   * - ``SYSREPO_MCP_SERVER_SYSREPO_DATASTORE_DIR``
     - ``/etc/sysrepo``
     - Repository path exported through ``SYSREPO_REPOSITORY_PATH``.
   * - ``SYSREPO_MCP_SERVER_SESSION_ID_LEN``
     - ``32``
     - Session identifier length in hexadecimal characters.

Session and schema settings
~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 20 40

   * - libconfig setting
     - Default
     - Description
   * - ``server.session.max_sessions``
     - ``64``
     - Concurrent MCP sessions (1-1024).
   * - ``server.session.ttl``
     - ``1800``
     - Idle expiry in seconds (60-86400).
   * - ``server.session.notif_queue_size``
     - ``256``
     - Notifications buffered per session (8-65536).
   * - ``server.session.default_timeout_ms``
     - ``5000``
     - Default timeout for sysrepo operations (100-60000).
   * - ``server.session.max_tree_depth``
     - ``32``
     - Hard schema traversal limit (1-256).

The session store is process-local, so the lighttpd configuration must use
``max-procs = 1``.

Authentication
~~~~~~~~~~~~~

The ``server.auth`` group accepts ``method`` (``none``, ``bearer`` or
``cookie``; default ``none``), ``cookie_name`` (default ``mcp_session``), and
``api_keys``. Each key entry is an object with a ``key`` and a sysrepo NACM
``user``. Authentication and NACM identity binding are active when
``method`` is ``bearer`` or ``cookie``. With ``method = "none"``, requests
run with the operating-system identity of the FastCGI process. Bearer or
cookie mode requires a configured key; missing or invalid credentials are
rejected.

Logging
~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 20 40

   * - libconfig setting
     - Default
     - Description
   * - ``server.log.syslog_enabled``
     - ``true``
     - Log to syslog through elog.
   * - ``server.log.file``
     - ``/var/log/sysrepo-mcp.log``
     - Append-only file log path; can be combined with other back ends.
   * - ``server.log.level``
     - ``6``
     - Syslog severity, 0 (emerg) to 7 (debug).
   * - ``server.log.verbose``
     - ``false``
     - Enable debug severity unless ``--log-level`` is supplied.
   * - ``server.log.console``
     - ``true``
     - Also log to ``stderr``.

Under FastCGI, ``stderr`` is captured by the web server or systemd. Configure
syslog or a writable file path for persistent application logs.

FastCGI transport
~~~~~~~~~~~~~~~~~

The application currently relies on lighttpd spawning it with ``bin-path``
and passing the listener on descriptor 0. Set ``max-procs`` to 1. A complete
configuration is provided at ``contrib/lighttpd/sysrepo-mcp.conf``; the
matching systemd unit starts lighttpd as the supervisor. There is no
standalone Unix or TCP listener in the current implementation, so the
transport socket fields formerly shown here did not have an effect.

Usage
-----

.. code-block:: bash

   sysrepo-mcp [--help] [--version] [-f <file>] [-l <severity>]

``--help`` prints a usage summary and exits, ``--version`` prints the version
and exits. ``--log-level`` accepts elog severities ``emerg``, ``alert``,
``crit``, ``err``, ``warn``, ``notice``, ``info`` or ``debug`` and overrides
the libconfig threshold. With no argument the process expects to be started
as a FastCGI application by a web server and exits with an error otherwise.

See :doc:`architecture` for the FastCGI deployment model. A lighttpd
configuration and matching systemd unit are included under ``contrib/``. The
unit runs lighttpd as ``sysrepo-mcp``; create that account, grant it the
permissions needed to access the sysrepo repository, and adjust ``Group`` to
the group used by the local sysrepo installation. Configure API keys and NACM
rules before enabling the service. Set ``server.log.file`` in the runtime
configuration to ``/var/log/sysrepo-mcp/sysrepo-mcp.log`` (or another path the
service account can write).

Install the example files as ``/etc/sysrepo-mcp/lighttpd.conf`` and
``/etc/systemd/system/sysrepo-mcp-lighttpd.service``, then run
``systemctl daemon-reload`` and enable the unit. It binds only to loopback on
port 8080; place a TLS reverse proxy in front of it for remote clients.

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
