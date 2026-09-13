.. include:: <isonum.txt>

Welcome to sysrepo-mcp-server documentation
##########################################

**Copyright** |copy| 2026 Loic JOURDHEUIL SELLIN.

This manual is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

This manual is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

A copy of the license is included in the section entitled
:ref:`GNU General Public License <gpl>`.

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
