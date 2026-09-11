################################################################################
# SPDX-License-Identifier: GPL-3.0-only
#
# This file is part of sysrepo-mcp-server.
# Copyright (C) 2025 Grégor Boirie <gregor.boirie@free.fr>
################################################################################

Integration Guide
=================

This chapter describes how to build and install sysrepo-mcp-server.

Prerequisites
-------------

Build requirements:

- eBuild system (extern/ebuild/ or /usr/share/ebuild/)
- GCC (version 8+) or Clang
- pkg-config
- sysrepo library (extern/sysrepo/)
- libyang (extern/libyang/)
- fcgi2 library (extern/fcgi2/)
- stroll library (extern/stroll/)
- utils library (extern/utils/)
- Sphinx documentation tools (for documentation build)

Installation Guide
------------------

Configuring the build
~~~~~~~~~~~~~~~~~~~~~

Configure the build options::

   make configure

This will generate a `.config` file in the build directory based on the
``config.in`` Kconfig template. You can customize the configuration using::

   make menuconfig

Building
~~~~~~~~

Build the project::

   make

Installing
~~~~~~~~~~

Install the binary and documentation (optional)::

   sudo make install

Cleanup
~~~~~~~

Clean build artifacts::

   make clean

Configuration Options
---------------------

The following configuration options are available in ``config.in``:

``CONFIG_SYSREPO_MCP_SERVER_LISTEN_ADDRESS``
   The address to listen on for MCP connections (default: ``127.0.0.1``)

``CONFIG_SYSREPO_MCP_SERVER_LISTEN_PORT``
   The TCP port to listen on (default: ``8080``)

``CONFIG_SYSREPO_MCP_SERVER_SYSLOG``
   Enable syslog logging (default: ``y``)

``CONFIG_SYSREPO_MCP_SERVER_VERBOSE``
   Enable verbose debugging output (default: ``n``)

``CONFIG_SYSREPO_MCP_SERVER_SYSREPO_SOCK_PATH``
   Sysrepo daemon socket path (default: ``/var/run/sysrepod.sock``)

``CONFIG_SYSREPO_MCP_SERVER_MCP_SOCK_PATH``
   MCP server socket path (default: ``/var/run/sysrepo-mcp.sock``)

Usage
-----

Start the server::

   sysrepo-mcp-server [--help] [--version]

The ``--help`` option prints a help message and exits. The ``--version`` option
prints the version number and exits.

License
-------

.. _gpl:

GPL License
~~~~~~~~~~~

This project is licensed under the terms of the GNU Lesser General Public
License version 3 or, at your option, any later version. A copy of the license
is available in the :ref:`license` appendix section.
