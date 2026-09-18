.. Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

.. _license:

License
=======

sysrepo-mcp is licensed under the **GNU Lesser General Public License, version
3.0** (LGPL-3.0-only).

The LGPL-3.0 is version 3 of the GNU General Public License supplemented by a
set of additional permissions. Both texts ship with the source tree:

``COPYING.txt``
   The GNU General Public License, version 3.

``COPYING.LESSER``
   The additional permissions that turn the above into the GNU Lesser General
   Public License, version 3.

Every source file carries an ``SPDX-License-Identifier: LGPL-3.0-only`` tag.

.. note::

   The summary below is informal and is **not** legal advice. Only the license
   texts shipped with the source tree are authoritative.

Why LGPL-3.0?
-------------

1. **Library friendly**: an application may link against sysrepo-mcp,
   including a proprietary one, as long as the user keeps the ability to
   relink against a modified version of the library.

2. **Copyleft on the library itself**: changes made to sysrepo-mcp remain
   available to its users.

3. **Compatible with the dependency stack**: the libraries sysrepo-mcp links
   against are distributed under permissive licenses (see below), which the
   LGPL-3.0 can incorporate.

Third-party licenses
--------------------

sysrepo-mcp links against, but does not include, the following libraries. Their
licenses apply to the corresponding binaries and must be honoured when
redistributing a build.

.. list-table::
   :header-rows: 1
   :widths: 20 25 55

   * - Library
     - License
     - Note
   * - libyang
     - BSD-3-Clause
     - YANG schema and data engine.
   * - sysrepo
     - BSD-3-Clause
     - YANG datastore API.
   * - json-c
     - MIT
     - JSON-RPC message parsing and generation.
   * - fcgi2
     - FastCGI open-market license
     - FastCGI transport (``libfcgi``).
   * - stroll, utils, elog
     - LGPL-3.0
     - eTux support libraries.
   * - eBuild
     - GPL-3.0
     - Build system only; not linked into the binary.

.. note::

   sysrepo and libyang are **not** LGPL: they moved to BSD-3-Clause. They can
   therefore be combined with LGPL-3.0 code without further constraints, but
   their copyright and license notices must be preserved in a redistribution.

Compliance checklist
--------------------

When distributing sysrepo-mcp, in binary or source form:

- Ship a copy of ``COPYING.txt`` and ``COPYING.LESSER``.
- Keep every copyright notice and SPDX tag intact.
- Document the changes made to sysrepo-mcp, and license them under LGPL-3.0.
- Provide the corresponding source, or a written offer to obtain it.
- Ship the license notices of the third-party libraries listed above.

When linking sysrepo-mcp into a larger application:

- Let users relink the application against a modified sysrepo-mcp, for example
  by using shared libraries or by shipping the object files.
- Do not impose terms that restrict the rights the LGPL-3.0 grants.

License text
------------

The full text of the additional permissions that define the LGPL-3.0:

.. include:: ../COPYING.LESSER
   :literal:

The full text of the GNU General Public License version 3, which the above
supplements:

.. include:: ../COPYING.txt
   :literal:

The canonical version of both documents is published at
https://www.gnu.org/licenses/lgpl-3.0.html.
