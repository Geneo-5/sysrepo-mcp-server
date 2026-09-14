.. Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

Integration Guide
=================

This chapter describes how to build and install sysrepo-mcp.

> **Note**: This project is a integration skeleton. No functionality is
> implemented at this stage. The build system is configured but the server
> code itself is a stub.

Prerequisites
-------------

Build requirements (all provided via Docker image):

- Docker with BuildKit
- eBuild system (extern/ebuild/ or /usr/share/ebuild/)
- GCC (version 8+) or Clang
- pkg-config
- sysrepo library (extern/sysrepo/)
- libyang (extern/libyang/)
- fcgi2 library (extern/fcgi2/)
- json-c library (extern/json-c/)
- stroll library (extern/stroll/)
- utils library (extern/utils/)

> **Note**: All dependencies are compiled and installed in the Docker image.
> No manual installation is required on the host system.

Installation Guide
------------------

Using Docker (Recommended)
~~~~~~~~~~~~~~~~~~~~~~~~~~

The **only supported build method** is via Docker. This ensures all dependencies
are correctly installed and configured.

1. **Build the Docker image**:

   .. code-block:: bash

      make -C docker build

   This command downloads all external sources to ``extern/`` and builds
the Docker image with all dependencies pre-installed.

2. **Run the build environment**:

   .. code-block:: bash

      make -C docker run

   This opens an interactive shell inside the build image with the project
   mounted at ``/home/builder/project``.

3. **Configure the build** (optional):

   .. code-block:: bash

      make config      # Opens menuconfig for Kconfig configuration

   or::

      make defconfig   # Generates default configuration without interactive UI

   Configuration options are defined in ``config.in`` and include:

   - Listen address and port for FastCGI
   - Sysrepo username and timeout
   - Logging level and verbosity

4. **Build the project**:

   .. code-block:: bash

      make

   This compiles the sysrepo-mcp binary to ``build/sysrepo-mcp``.

5. **Install** (optional):

   .. code-block:: bash

      sudo make install

   This installs the binary to ``/usr/local/bin/sysrepo-mcp``.

6. **Clean build artifacts**:

   .. code-block:: bash

      make clean

Configuration Options
---------------------

The following configuration options are available in ``config.in`` (Kconfig format).
Use ``make config`` (menuconfig) or ``make defconfig`` to set these options.

Server Configuration
~~~~~~~~~~~~~~~~~~~~

``SYSREPO_MCP_SERVER_MCP_PATH``
   MCP endpoint path (URL path for FastCGI). Default: ``"/mcp"``

``SYSREPO_MCP_SERVER_SESSION_TTL``
   Session timeout in seconds (idle sessions are destroyed after this time).
   Range: 60-86400. Default: ``1800``

``SYSREPO_MCP_SERVER_MAX_SESSIONS``
   Maximum number of concurrent MCP sessions. Range: 1-1024. Default: ``64``

FastCGI Transport
~~~~~~~~~~~~~~~~

``SYSREPO_MCP_SERVER_TRANSPORT_UNIX``
   Enable Unix socket transport (for local connections). Default: ``y``

``SYSREPO_MCP_SERVER_TRANSPORT_TCP``
   Enable TCP transport (for remote or proxied connections). Default: ``n``

``SYSREPO_MCP_SERVER_UNIX_SOCKET_PATH``
   Unix socket path for FastCGI. Default: ``"/var/run/sysrepo-mcp.sock"``
   (Only used when ``SYSREPO_MCP_SERVER_TRANSPORT_UNIX=y``)

``SYSREPO_MCP_SERVER_TCP_HOST``
   TCP host to bind for FastCGI. Default: ``"127.0.0.1"``
   (Only used when ``SYSREPO_MCP_SERVER_TRANSPORT_TCP=y``)

``SYSREPO_MCP_SERVER_TCP_PORT``
   TCP port to bind for FastCGI. Range: 1-65535. Default: ``8080``
   (Only used when ``SYSREPO_MCP_SERVER_TRANSPORT_TCP=y``)

Sysrepo Connection
~~~~~~~~~~~~~~~~~~

``SYSREPO_MCP_SERVER_SYSREPO_USERNAME``
   Username used when creating sysrepo sessions (``sr_session_create()``).
   Default: ``"mcp"``

``SYSREPO_MCP_SERVER_SYSREPO_TIMEOUT``
   Sysrepo operation timeout in milliseconds. Range: 1000-60000. Default: ``5000``

``SYSREPO_MCP_SERVER_SYSREPO_DATSTORE_DIR``
   Path to sysrepo datastore directory. Default: ``"/etc/sysrepo/data"``

Logging Configuration
~~~~~~~~~~~~~~~~~~~~~~

``SYSREPO_MCP_SERVER_SYSLOG_ENABLED``
   Enable syslog logging via elog library. Default: ``y``

``SYSREPO_MCP_SERVER_LOG_LEVEL``
   Log level (0=emerg, 1=alert, 2=crit, 3=err, 4=warning, 5=notice, 6=info, 7=debug).
   Range: 0-7. Default: ``6``

``SYSREPO_MCP_SERVER_LOG_VERBOSE``
   Enable verbose debugging output. Default: ``n``

``SYSREPO_MCP_SERVER_LOG_CONSOLE``
   Enable console logging (stderr). Default: ``y``

Usage
-----

Start the server (after building):

.. code-block:: bash

   ./build/sysrepo-mcp [--help] [--version]

The ``--help`` option prints a help message and exits. The ``--version`` option
prints the version number and exits.

> **Note**: The server currently only prints help/version. All MCP functionality
> is planned but not yet implemented in this skeleton.

Dependencies Details
--------------------

The following libraries are used by sysrepo-mcp:

.. list-table::
   :header-rows: 1
   :widths: 20 15 65

   * - Library
     - Version
     - Purpose
   * - **fcgi2**
     - 2.4.7
     - FastCGI transport (mandatory)
   * - **json-c**
     - 0.16+
     - JSON parsing/generation for MCP messages
   * - **libyang**
     - 5.8.6
     - YANG schema parsing and validation
   * - **sysrepo**
     - 5.1.0
     - NETCONF datastore API (library mode)
   * - **stroll**
     - master
     - Data structures (lists, hashes, buffers)
   * - **utils**
     - master
     - eTux utilities (file, network, thread, etc.)
   * - **ebuild**
     - master
     - Build system (Makefile framework)

> **Important**: All dependencies are provided via the Docker image.
> The ``extern/`` directory contains source references for IA agents and
> is used to build the Docker image. It is excluded from Git and must never
> be modified manually.

License
-------

This project is licensed under the terms of the GNU Lesser General Public
License version 3 (LGPL-3.0). A copy of the license is available in the
:ref:`license` appendix section.
