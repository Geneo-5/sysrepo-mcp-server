.. include:: <isonum.txt>

Welcome to sysrepo-mcp documentation
##########################################

sysrepo-mcp is a Model Context Protocol (MCP) server that bridges AI agents
with sysrepo's NETCONF configuration datastore. This documentation describes
the architecture, installation, and API of sysrepo-mcp.

> **Note**: This project is currently a **skeleton**. The documentation describes
> the target implementation. No functionality is implemented at this stage.

For license information, see the :ref:`license` section.

.. Caption of toctrees are not translated into latex, hence the dirty trick
.. below. See https://github.com/sphinx-doc/sphinx/issues/3169 for more infos.
.. Basically, we ask the latex backend to generate a \part{} section for each
.. toctree caption using the `raw' restructuredtext directive.

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


.. We use the latex_appendices setting into conf.py to benefit from native latex
.. appendices section numbering scheme. As a consequence, there is no need to
.. generate appendix entries for latex since already requested through the
.. latex_appendices setting.

.. toctree::
   :maxdepth: 2
   :caption: Appendices

   license
   todo
   genindex
