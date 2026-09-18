.. Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

.. include:: <isonum.txt>

Welcome to sysrepo-mcp documentation
####################################

sysrepo-mcp is a `Model Context Protocol <https://modelcontextprotocol.io>`_
(MCP) server that bridges AI agents with the `sysrepo
<https://github.com/sysrepo/sysrepo>`_ YANG datastore. It exposes sysrepo
operations (read and edit configuration, read operational state, run RPCs and
actions, explore YANG schemas) as MCP tools, over a FastCGI transport served by
a reverse proxy.

.. warning::

   **Work in progress.** The build system, the Docker environment and the
   documentation describe the target design. The server itself is a partial
   skeleton: several tools are stubbed and the MCP lifecycle
   (``initialize`` / ``tools/list``) is not implemented yet. Every section
   below states explicitly what is implemented and what is planned; the
   :doc:`todo` appendix tracks the remaining work.

For license information, see the :ref:`license` appendix.

.. only:: latex

   .. raw:: latex

      \part{Integration Guide}

.. toctree::
   :numbered:
   :caption: Integration Guide

   install
   architecture

.. only:: latex

   .. raw:: latex

      \part{API Guide}

.. toctree::
   :maxdepth: 2
   :numbered:
   :caption: API

   api

.. toctree::
   :maxdepth: 2
   :caption: Appendices

   license
   todo

.. only:: html

   Indices
   =======

   * :ref:`genindex`
   * :ref:`search`
