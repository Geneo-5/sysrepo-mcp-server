.. Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

Roadmap
=======

This appendix is the single source of truth for the state of the project. It
tracks what works, what does not, and in which order the remaining work should
be done.

.. note::

   Keep this page and the reality of the tree in sync. A roadmap that claims
   more than the code delivers is worse than no roadmap at all: it is what
   sends a reader looking for a feature that was never written.

Where the project stands
------------------------

Done
~~~~

- Project skeleton: ``Makefile``, ``ebuild.mk``, ``config.in``, layout.
- Docker build environment: every dependency compiled from ``extern/`` and
  installed under ``/usr/local``.
- ``docker/Makefile`` targets to download, build and run.
- ``scripts/build-docker.sh`` to drive the whole thing from the host.
- Kconfig options in ``config.in``, restricted to build-time settings only
  (session limits, logging, transport moved to runtime libconfig file).
- Runtime configuration via libconfig file (``src/libconfig.c``, ``src/config.c``,
  ``src/main.c``), parsed at startup with ``mcp_config_set()`` /
  ``mcp_config_get()``; default config in ``docker/sysrepo-mcp.conf``.
  YANG module ``yang/sysrepo-mcp.yang`` removed; no module installed into sysrepo.
- Session allocation no longer statically sized (``CONFIG_SYSREPO_MCP_SERVER_MAX_SESSIONS``
  gone); ``sessions_init()`` allocates dynamically from the config value.
- Sphinx documentation: installation, architecture, API reference, license.
- Test suite: 259 tests verified in Docker over HTTP through lighttpd, including
  the upstream oven plugin.
- FastCGI transport, JSON-RPC framing and the HTTP status contract.
- Runtime transport modes: proxy-provided FastCGI descriptor, standalone
  FastCGI listener on a Unix socket or numeric IPv4 address/port.
- MCP protocol: legacy ``2025-11-25`` sessions and stateless
  ``2026-07-28`` discovery, tool listing/calls, metadata, and cache fields.
- Sessions: ``Mcp-Session-Id``, idle expiry, maximum count, ``DELETE``.
- Datastore tools: ``sr_get_config``, ``sr_edit_config``,
  ``sr_delete_config``, ``sr_copy_config``, ``sr_get_operational``.
- Operation tools: ``sr_execute_rpc``, ``sr_action``.
- Notification tools: ``sr_notif_subscribe``, ``sr_notif_unsubscribe``,
  ``sr_notif_list_subscriptions``, ``sr_notif_poll``, ``sr_notif_send``.
- Module tools: ``sr_list_modules``, ``sr_module_install``,
  ``sr_module_uninstall``.
- Introspection: ``get_status``, ``get_schema``.
- ``SR_ERR_*`` mapped onto distinct JSON-RPC codes.
- Source file split: ``main.c`` (FastCGI entry point, tool catalogue),
  ``config.c`` (``mcp_config_set()``/``mcp_config_get()``, sysrepo_open/close),
  ``libconfig.c`` (libconfig file parser into ``struct mcp_config``),
  ``sessions.c`` (session CRUD, notification queue, subscription management),
  ``transport.c`` (HTTP/RPC plumbing, MCP methods, request dispatch),
  ``utilities.c`` (cross-cutting helpers: ``tree_to_json``, argument
  extraction, ``tool_find``), and one source + header per functional area
  (config_tools, operational, rpc, notifications, modules, schema, status).

Priority backlog
-----------------

Only unfinished work appears here, ordered by impact. Completed milestone
checklists were removed from this section; their delivered behavior is listed
under *Done* above and documented in the corresponding API/architecture pages.

P0 — Security and fail-closed startup
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. **Make API-key lookup constant-time.** ``mcp_config_find_key()`` currently
   compares every byte up to the longer input, so runtime still depends on
   key length. Use a fixed maximum key length and scan the full configured
   credential set without returning early on a byte or key match. Include
   length mismatch in the accumulated result. Reject configured keys beyond
   the maximum. Add tests for a valid key, a same-prefix wrong key, shorter
   and longer keys, and matches at the first and last configured entries.
   Do not hash keys: the user explicitly rejected SHA-256 as unnecessary CPU
   cost. Keys therefore remain plaintext in the config file and process
   memory; require high-entropy values, restrictive config-file permissions,
   and never log credentials.

2. **Fail closed on invalid configuration.** ``main()`` currently warns when
   ``mcp_config_load()`` fails and continues with defaults; this can disable
   configured authentication after a syntax or validation error. Exit before
   opening sysrepo or a listener on parse/validation errors. Preserve built-in
   defaults only for the documented case where the default config file is
   absent, and test both cases.

P1 — Interoperability and deployment verification
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. **Make schema discovery progressive for agents without source YANG.**
   Field feedback reports that a full ``get_schema(xpath="/")`` response is
   about 72 KB and that guessing ``/oven`` or the YANG prefix path
   ``/ov:oven`` fails. The current API already supports the solution: start
   with ``max_depth: 1``, read the exact node ``xpath`` returned, then drill
   into that subtree. The YANG prefix is metadata; the XPath first segment
   uses the module name (for the local fixture, module ``oven``, prefix
   ``ov``, path ``/oven:oven``). This flow is now documented and covered by
   ``test_schema_can_be_explored_progressively_without_source``. Improve
   invalid-path feedback to point to discovered root paths or this workflow,
   and confirm it works with an agent using only MCP tools.

2. **Verify the live MCP connector after deployment.** The connector reported
   that ``tools/list`` rejected missing ``ttlMs`` and ``cacheScope``. The
   server now includes both fields (``ttlMs: 0``, ``cacheScope: "private"``)
   in modern ``tools/list`` and ``server/discover`` responses; the full Docker
   suite passes (259 passed, 0 failed). Restart/redeploy the updated responder
   and confirm the connector can fetch and invoke a read-only tool. This
   session currently cannot call the connection's tools, so live acceptance
   remains unverified.

3. **Correct the authentication error code.** A missing or invalid API key
   returns HTTP 401 with JSON-RPC ``-32603`` (Internal error). Choose the
   documented authentication/authorization code, update both initialization
   and stateless request paths, and align tests and :doc:`api`. Do not leave a
   client authentication failure classified as an internal server fault.

P2 — Protocol and agent capabilities
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. **Modern notification subscriptions.** ``2026-07-28`` tool listings omit
   the session-bound subscribe/unsubscribe/list/poll tools, so these remain
   available only to legacy ``2025-11-25`` clients. Define a stateless
   subscription identity/lifecycle compatible with the modern protocol, or
   explicitly retain this as a documented legacy-only capability with an
   interoperability test.
2. **Trusted browser clients.** Requests with ``Origin`` are rejected by
   default. If browser clients are required, add an explicit origin allow-list
   in runtime configuration, validate it before processing requests, and test
   allowed, rejected, and absent origins. Keep the default deny behavior.
3. **Implement ``sr_diff_config``.** Compare two conventional datastores and
   return the documented flat ``diff`` array. Read both trees with
   ``sr_get_data()`` and compute changes with ``lyd_diff_siblings()``.

   a. Build ``diff_to_json()`` using the XPath construction logic shared with
      ``tree_to_json()``.
   b. Decide and document whether depth limits apply during reads or after
      diffing; the current API description assumes the former, which can hide
      changes below the cutoff.
   c. Include ``previous_position`` for moved entries in user-ordered lists
      and leaf-lists using libyang's ``yang:key``/``yang:value``/
      ``yang:position`` metadata.
   d. Reject identical source and target datastores with ``-32602``.
   e. Test changed leaves, added/removed/reordered list entries, and no-op
      results.

P3 — Verification and release quality
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. Add CI coverage for a matrix of supported libyang/sysrepo revisions. They
   are pinned together; introspection is the first area likely to break when
   either revision changes.
2. Run the server and exercised tests under Valgrind, focusing on JSON
   reference ownership and notification queue lifetimes.

P4 — Optional future capabilities
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. Transactions spanning several tool calls, with explicit commit and rollback.
2. Metrics export.

Not planned: OAuth2/JWT, a dedicated dry-run tool, SSE, WebSocket, or a direct
HTTP listener. See *Not planned* below for the recorded decisions.

Environment d'agent
-------------------

Bilan tiré de l'utilisation pratique du serveur sysrepo-mcp comme environnement
d'exécution d'outils. Objectif : identifier ce qui rend cet environnement
rapide et facile à utiliser. Les points d'amélioration identifiés ici sont
suivis dans le *Priority backlog* ci-dessus, regroupés par urgence et impact.

Points positifs
~~~~~~~~~~~~~~~

- **Interface unifiée.** Un seul protocole (JSON-RPC sur FastCGI) pour tout :
  lecture/écriture de datastore, appels RPC, notifications, gestion de modules.
  Un agent n'a pas à mémoriser plusieurs API.

- **Introspection riche.** ``get_status`` et ``get_schema`` donnent accès au schéma YANG compilé, aux
  valeurs par défaut, aux plages, aux patterns et aux descriptions. On peut naviguer un module inconnu
  sans lire la documentation.

- **XPath stable.** Le XPath est l'identifiant canonique de chaque nœud : pas
  d'IDs aléatoires, pas de lookup par nom. Un agent qui apprend le XPath d'un
  nœud le retrouve toujours, dans n'importe quel datastore.

- **Multi-datastore.** ``running``, ``startup`` et ``candidate`` sont
  accessibles directement. Un agent peut lire, valider et valider avant
  d'appliquer.

- **Notifications complètes.** Cycle complet géré par le serveur :
  ``sr_notif_subscribe``, ``sr_notif_poll``, ``sr_notif_unsubscribe``,
  ``sr_notif_send``. Aucun thread par défaut : l'agent poll à chaque requête,
  ce qui évite la concurrence sur la file d'attente.

- **Sessions MCP.** Cycle de vie géré (identification, TTL d'inactivité,
  expiration, ``DELETE``). Un agent peut lancer plusieurs sessions parallèles.

- **Cartographie des erreurs.** ``SR_ERR_*`` mappés sur des codes
  JSON-RPC distincts. Un agent peut distinguer une erreur de validation d'une
  erreur interne et agir en conséquence.

- **Intégration YANG native.** ``sysrepo-mcp`` est linké contre libyang et
  sysrepo. Les modifications du schéma se répercutent directement dans ``get_schema``.

Build system
------------

All of the following were fixed; they are kept here as a record of what the
build used to get wrong.

- ``ebuild.mk``: ``libyang`` was missing from the ``pkg-config`` call while
  the code called ``lyd_*`` and ``lys_*``.
- ``ebuild.mk``: the ``pkg-config`` failure was silenced with ``2>/dev/null``,
  turning a missing ``.pc`` file into a confusing link error.
- ``docker/Dockerfile``: a mid-file
  ``ENV PKG_CONFIG_PATH=/usr/lib/pkgconfig`` hid
  ``/usr/local/lib/pkgconfig``, where libyang and sysrepo install their
  ``.pc`` files.
- ``docker/Makefile``: ``$(id -u)`` was expanded by make, not by the shell,
  and yielded an empty ``-u ":"``.
- ``.github/workflows/c-cpp.yml``: the script path was wrong, and the workflow
  had never run to completion.
- ``libconfig-dev`` and the stale ``docker/config.cfg`` were dropped: the
  project did not use libconfig at the time. Runtime configuration now uses
  libconfig; only build-time toggles remain in Kconfig.
- ``config.in`` Kconfig entries for session limits, transport, authentication,
  access control, and logging moved to runtime; only build-time toggles
  (session ID length) remain in Kconfig.  Removed
  ``SYSREPO_MCP_SERVER_DEFAULT_TIMEOUT_MS`` and
  ``SYSREPO_MCP_SERVER_MAX_TREE_DEPTH`` — now pure runtime fields.

Source layout
-------------

``main.c`` was split into 10 source files + 10 headers.

``src/main.c``
   FastCGI entry point (``FCGX_Accept_r`` loop, signal handling,
   ``sysrepo_open/close``), ``--config`` CLI option, libconfig loading,
   ``sysrepo-mcp`` global runtime config accessor.

``src/config.c``
   Global runtime config storage (``g_config``), ``mcp_config_set()`` and
   ``mcp_config_get()``.  Calls ``mcp_config_load()`` internally (no Kconfig
   parsing).

``src/libconfig.c``
   Parse a libconfig file into ``struct mcp_config`` with ``mcp_config_load()``,
   apply defaults with ``mcp_config_set_defaults()``, free with
   ``mcp_config_free()``.  API key list parsing, range validation.

``src/sessions.c``
   Session data structures, CRUD, notification queue, subscription management.

``src/transport.c``
   HTTP/RPC plumbing (``http_send``, ``rpc_send``), MCP methods
   (``initialize``, ``tools/list``, ``tools/call``), request dispatch,
   FastCGI ``serve()``.

``src/utilities.c``
   Cross-cutting helpers: ``tree_to_json``, ``nodetype_to_json``, argument
   extraction (``arg_string``, ``arg_object``, ``arg_int``, ``arg_bool``,
   ``arg_xpath``, ``arg_datastore``), ``xpath_wellformed``, ``tool_find``,
   ``tool_content``.

``src/config_tools.c``
   ``sr_get_config``, ``sr_edit_config``, ``sr_delete_config``,
   ``sr_copy_config``.

``src/operational.c``
   ``sr_get_operational``.

``src/rpc.c``
   ``sr_execute_rpc``, ``sr_action``, ``rpc_common``.

``src/notifications.c``
   All notification tools: subscribe, unsubscribe, list_subscriptions,
   poll, send.

``src/modules.c``
   ``sr_list_modules``, ``sr_module_install``, ``sr_module_uninstall``.

``src/schema.c``
   ``get_schema``, its recursive walker, and ``basetype_name``.

``src/status.c``
   ``get_status``.

Headers live under ``include/sysrepo/mcp/`` — one per source file, forward
declarations only. Every module includes ``utilities.h`` for shared helpers.

Testing
-------

The suite in ``tests/`` drives the server the way a client does: HTTP to
lighttpd on port 80, forwarded over FastCGI, against the upstream oven plugin
from ``extern/sysrepo/examples/plugin/oven.c``. It runs in a private sysrepo
repository and shared-memory namespace, so it never touches the system
repository. Remaining test work is tracked in *P4 — Tests to complete* above.

Layout:

``tests/conftest.py``
   Fixtures: the private repository, module installation, lighttpd in front of
   the server, and the oven plugin. Each layer skips with an explicit reason,
   so a partial environment still runs the tests it can.

``tests/test_transport.py``
   HTTP and JSON-RPC framing: status codes, headers, malformed bodies,
   notifications, and the guarantee that every request gets an answer.

``tests/test_protocol.py``
   MCP lifecycle, the tool catalogue and its input schemas, the ``content``
   envelope, ``get_status``.

``tests/test_oven.py``
   The oven end to end: configuration reads and writes, refused writes,
   operational state from the plugin, the RPCs, and one full cooking session.

``tests/test_sessions.py``
   Sessions and notifications: the ``Mcp-Session-Id`` lifecycle, isolation
   between sessions, subscribing, polling, draining, and the full watch-a-
   device flow against ``oven-ready``.

``tests/test_schema.py``
   ``get_schema`` against whole modules, selected subtrees, depth limits, and RPC inputs.

``tests/test_errors.py``
   Argument validation, the ``SR_ERR_*`` mapping, module listing, and the
   project's own YANG module, which supplies the list, key predicate and
   empty-match cases that the oven model has none of.

Not planned
-----------

Decisions already taken, recorded here so they are not re-litigated:

**SSE and server-initiated messages**
   The server answers POSTs with a single JSON object. This is permitted by
   the MCP Streamable HTTP binding, and it rules out MCP sampling and
   elicitation. Sysrepo notifications are not lost, but they are queued and
   polled rather than pushed.

**WebSocket transport**
   Not an MCP binding.

**A direct HTTP listener**
   HTTP, TLS and rate limiting belong to the reverse proxy.

**Multiple concurrent agent sessions / a shared session store**
   This is an MCP server for configuring one complex system, not a
   multi-tenant service. Running several agents against it at once risks
   conflicting, interleaved configuration changes on the same datastore —
   the opposite of what the server exists to prevent. Sessions therefore
   stay local to a single FastCGI process, and ``max-procs = 1`` is the
   intended deployment, not a throughput ceiling to be lifted. Horizontal
   scaling and a shared session store are out of scope for the same reason.

**OAuth2 / JWT authentication**
   The MCP authorization specification builds on OAuth2/JWT, but adopting it
   here would pull in a token-issuing/validation stack for a service meant
   to stay small, simple, and with minimal footprint on the sysrepo
   environment it sits next to. The libconfig API-key list plus NACM (see
   P0) is the deliberately lighter mechanism this project uses instead, and
   that trade-off is not expected to change.

**A dedicated dry-run tool**
   sysrepo itself has no dry-run operation to wrap. Writing to ``candidate``,
   validating it, and comparing it against ``running`` already gets an agent
   most of the way there — see ``sr_diff_config`` in :doc:`api` (P2) once it
   exists. ``sr_copy_config`` can promote a validated ``candidate``. A
   separate dry-run tool would only be
   reconsidered if that combination proves insufficient in practice.

Contributing
------------

1. Pick the earliest incomplete item from the *Priority backlog* above.
2. Read :doc:`architecture` for the technical context.
3. Follow the existing style, and build in the container.
4. Add a test that fails without the change.
5. Update this page and the affected documentation in the same commit.
