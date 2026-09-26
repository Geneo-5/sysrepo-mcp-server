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
- Test suite: 260-odd tests over HTTP through lighttpd, against the upstream
  oven plugin.
- FastCGI transport, JSON-RPC framing and the HTTP status contract.
- MCP lifecycle: ``initialize``, ``notifications/initialized``,
  ``tools/list``, ``ping``, and the ``content`` result envelope.
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

Everything still to do, across the whole project (milestones, agent-usage
gaps, tests, future work), gathered here in a single ordered list. Each item
below used to live in its own section; those sections now only record
context and decisions, not open work — see the cross-references.

Running several agent sessions concurrently against one server is not a
goal here — see *Not planned* below — so nothing in this backlog is about
lifting ``max-procs = 1``.

Findings from this review (not yet triaged into a priority)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- ``config.in``'s top-of-file comment ("Configuration options that affect
  runtime behavior are in ``yang/sysrepo-mcp.yang``") referred to the module
  P1.1 removed. Fixed: both occurrences now point at the libconfig file.
- ``README.md``'s "État du projet" callout still listed authentication and
  NACM as missing, which contradicted both this page (P0 marked done) and
  ``sphinx/architecture.rst`` ("fully enforced"). P0.10 is now resolved, so
  this has been settled: ``README.md``, ``AGENTS.md`` and ``sphinx/api.rst``
  updated to match the code.
- **P0.3 ("Store keys hashed") is marked done above but the code does not do
  it.** ``load_auth()`` in ``src/libconfig.c`` copies ``key`` straight from
  the libconfig file with ``xstrdup()`` into ``struct mcp_api_key.key``
  (``include/sysrepo/mcp/libconfig.h``); ``mcp_config_find_key()`` compares
  it byte-by-byte against the presented credential, which does resist a
  timing attack, but the key sits in cleartext in the config file and in the
  process's memory the whole time. Either re-open P0.3 (hash at load time,
  compare against the hash) or reword it to describe what is actually
  implemented (byte-by-byte comparison, not hashing) — the strikethrough
  currently overstates the security property to a reader who has not read
  the source.
- ``sphinx/api.rst``'s error-code documentation (implementation-defined
  range and the sysrepo mapping table) did not match
  ``include/sysrepo/mcp/utilities.h`` at all — the codes were entirely
  transposed. Fixed. One remaining gap surfaced while fixing it: a missing
  or unknown API key is reported as ``-32603`` (Internal error) with HTTP
  401, in both ``method_initialize()`` and ``serve()`` in ``transport.c``.
  ``-32603`` is meant for unexpected server-side failure, not a client
  authentication problem; ``-32007`` is unused and would be a natural home
  for it, but this needs an explicit decision (and a test) before changing
  the wire contract.
- ``sphinx/architecture.rst`` is the most out of date of the three docs:
  its own "Authentication" section still says "Not implemented" and
  describes looking the key up in a removed YANG module
  (``/sysrepo-mcp:api-key``), and "Configuration model" still lists the API
  key list as living in ``yang/sysrepo-mcp.yang``. Both contradict the
  chapter's own opening warning, which already says authentication is
  implemented. Needs the same reconciliation pass as the other three docs.

**Recent test runs exposed regressions; their fixes still need a green rerun.**
One run reported 269 passing and three failures: the NACM-denied RPC returned
``-32001`` because the isolated auth repository did not have ``oven``
installed, and two oven read-back assertions lost ``false``/default-valued
leaves after ``LYD_PRINT_WD_TRIM`` was made the implicit printer flag. The
auth fixture now installs ``oven``, and ``sr_get_config`` preserves the old
output when ``options`` is omitted. A subsequent run reported three protocol
failures because ``sr_get_config``'s malformed input-schema JSON was exposed
as an empty schema; the catalogue string was corrected. These corrections
have not yet been verified by a complete passing run.

**Live oven audit not yet performed.** The user reports adding a sysrepo-mcp
connection, but this Codex session exposes no sysrepo-mcp tool or resource:
the callable tool list has no sysrepo entry, and the MCP resource list only
contains plugin-management resources. Record runtime findings after that
connection is available to the agent; do not infer live behavior from the
local test fixtures.

P0 — Security (blocks any untrusted deployment)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. warning::

   Until this is complete, an agent has every right of the system user
   running the server. The build must not be exposed to an untrusted agent,
   and the documentation must keep saying so. This is the single most
   critical risk in the project today, ahead of scaling.

1. ~~Extract the credential from ``HTTP_AUTHORIZATION`` or a cookie, per
   ``SYSREPO_MCP_SERVER_AUTH_BEARER`` / ``SYSREPO_MCP_SERVER_AUTH_COOKIE``
   and ``SYSREPO_MCP_SERVER_COOKIE_NAME`` in the libconfig file (see P1).~~
2. ~~Look the key up in the API-key list of the libconfig configuration file
   (see P1) and resolve the NACM user — no longer ``/sysrepo-mcp:api-key``
   in the datastore.~~
3. ~~Store keys hashed, and compare in constant time.~~
4. ~~``sr_nacm_init()`` at startup, ``sr_nacm_set_user()`` per request,
   ``sr_nacm_check_operation()`` before an RPC, ``sr_nacm_destroy()`` at exit
   — gated by ``SYSREPO_MCP_SERVER_ACL_ENABLE_NACM``.~~
5. ~~Apply the module allow-list (``SYSREPO_MCP_SERVER_ACL_ALLOWED_MODULES``,
   gated by ``SYSREPO_MCP_SERVER_ACL_ENABLE_MODULE_FILTER``), the operation
   filter (``SYSREPO_MCP_SERVER_ACL_ENABLE_OPERATION_FILTER``) and the
   write protection (``SYSREPO_MCP_SERVER_ACL_ENABLE_WRITE_PROTECTION``)
   from the libconfig file before calling sysrepo — all under the master
   ``SYSREPO_MCP_SERVER_ACL_ENABLED`` switch (see P1).~~
6. ~~Bind the identity to the session, so a subscription cannot outlive the
   rights that created it.~~
7. ~~Deny ``sr_module_install`` and ``sr_module_uninstall`` by default. They
   change the schema of the whole datastore for every process linked against
   sysrepo, and removing a module destroys its data. (Revisit once
   authentication exists — see P3.4.)~~
8. ~~Log every configuration change with the identity that caused it.~~
9. ~~Cover authentication and NACM with tests once they exist. Until then
   there is nothing to assert beyond "everything is permitted", which is
   exactly the state the tests must not enshrine.~~
10. ~~Reopened by this review. ``sr_nacm_check_operation()`` is not
    called anywhere in the source tree. ``sr_nacm_set_user()`` is set on
    the session (item 6), so sysrepo's own data-level enforcement covers
    ``sr_get_config``/``sr_edit_config``/``sr_delete_config``, but
    ``rpc_common()`` in ``rpc.c`` sends the operation straight to
    ``sr_rpc_send_tree()`` with no explicit authorization check first —
    the code says so itself, in the comment above the call in
    ``method_tools_call()`` (``transport.c``): "deferred to a follow-up
    commit". Until this lands, an authenticated but unprivileged agent can
    invoke any RPC or action of any installed module. Add the missing
    ``sr_nacm_check_operation()`` call in ``rpc_common()`` before
    ``sr_rpc_send_tree()``, and a test asserting a NACM-denied user gets
    ``-32003`` on an RPC it may not call. Item 4 above should not be read
    as covering this case.~~ (Done: ``sr_nacm_init()`` au démarrage dans
    ``sysrepo_open()``, ``sr_nacm_check_operation()`` dans
    ``rpc_common()`` avant ``sr_rpc_send_tree()``,
    ``sr_nacm_destroy()`` à l'arrêt dans ``sysrepo_close()``,
    test ``test_nacm_denied_rpc()``.)

P1 — Configuration: drop the YANG module, adopt libconfig
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Installing this MCP server should not itself install a YANG module or
otherwise change what the sysrepo instance offers. Runtime configuration
(API keys, session limits, logging) belongs in a config file read once at
process startup, not in the datastore.

1. ~~Remove ``yang/sysrepo-mcp.yang`` entirely; the server no longer installs
   any YANG module of its own into sysrepo.~~ (Done: ``yang/`` no longer
   contains ``sysrepo-mcp.yang``.)

2. ~~Introduce a libconfig-based configuration file, parsed once at startup,
   carrying every setting that is a runtime concern rather than a
   build-time toggle.~~ (Done: ``src/libconfig.c`` + ``src/config.c`` parse
   ``docker/sysrepo-mcp.conf``, parsed by ``main.c`` at startup.)

3. Everything else in ``config.in`` — paths, feature toggles compiled in —
   stays build-time Kconfig; the split is settled by the list above, so
   this is no longer an open question.
4. Update ``sysrepo_open/close`` in ``config.c`` to parse the libconfig
   file instead of reading sysrepo-mcp's own datastore subtree for this
   data. (Done: ``src/config.c`` calls ``mcp_config_load()`` and
   ``mcp_config_set()``.)
5. ~~Rework the P0 authentication design accordingly: the API key list and
   the hashed comparison move to the config file; only
   ``sr_nacm_set_user()`` and the NACM check still go through sysrepo
   (NACM itself is sysrepo's own mechanism, not this project's YANG
   module).~~ (Done: all P0 items completed, ``docker/sysrepo-mcp.conf``
   provides the API key list at runtime.)
6. ~~Rewrite ``tests/test_errors.py``'s list, key-predicate and empty-match
   coverage against a different fixture module, since it currently relies
   on the project's own YANG module for cases the oven model doesn't have.~~
   (Done: ``docker/sysrepo-mcp.conf`` provides a test fixture with no API
   keys, which is the expected test state.)
7. ~~Update the docs and ``docker/Dockerfile``: nothing under ``yang/`` to
   install for this project's own schema; ``sr_module_install`` stays only
   for modules the deployment itself chooses to manage (still denied by
   default, see P0.7).~~ (Done: ``docker/sysrepo-mcp.conf`` in place, default
   config falls back to built-in values when the file is absent.)
8. Remove session limits, transport, authentication, access control, logging
   entries from ``config.in`` Kconfig (``MAX_SESSIONS``, ``SESSION_TTL``,
   ``NOTIF_QUEUE_SIZE``, ``DEFAULT_TIMEOUT_MS``, ``MAX_TREE_DEPTH`` moved to
   runtime only). (Done: ``mcp_config_set_defaults()`` provides the same
   defaults.)

P2 — Agent-usability gaps
~~~~~~~~~~~~~~~~~~~~~~~~~

Smaller than a milestone individually, but each one currently forces an
agent to guess or work around a limitation.

1. ~~**Merge get_tree and get_help into one xpath/depth-driven tool.**~~
   The former ``get_tree`` and ``get_help`` tools have been replaced by
   ``get_schema``, which shares the optional ``xpath`` and ``max_depth`` arguments:

   - ``xpath`` (string, optional). Starting point of the walk. Omitted or
     ``"/"`` means the datastore root — every implemented module, not just
     one. ``module`` is dropped as a separate argument.
   - ``max_depth`` (integer, optional, default ``0``). ``0`` means
     unlimited recursion from ``xpath``; a positive value stops N levels
     below the start node. The existing build-time
     ``mcp_config_get()->max_tree_depth`` stays as a hard safety ceiling:
     effective depth is ``max_depth == 0 ? config limit : min(max_depth,
     config limit)``, so a caller cannot force unbounded recursion on a
     pathological schema.

   Completed sub-tasks:

   a. ~~Factor recursion into a depth-aware node walker and per-node
      callback.~~
   b. ~~Put the full XPath on every recursive node and drop the duplicate
      flat ``nodes`` array.~~
   c. ~~``get_tree``: extend the per-node JSON towards a transcription of
      ``LYS_OUT_TREE`` (the shape ``yanglint -f tree`` prints). In addition
      to the existing ``type``/``config``, add ``mandatory``, cardinality
      for lists and leaf-lists (``min-elements``/``max-elements``), the key
      list of a list, whether a container is ``presence``, choice/case
      grouping, and whether the node comes from an ``augment``. This is new
      information, not a reshuffle of what ``get_help`` already computes.~~
      Implemented in the current tree output.
   d. ~~``get_help`` → ``get_schema``: move ``add_leaf_help()`` and
      ``res_add_range()`` (already correct for leaf/leaf-list) into the
      shared walker so every visited node — not only the one named by
      ``xpath`` — carries its full compiled detail: the JSON equivalent of
      ``LYS_OUT_YANG_COMPILED``. While doing this, generalize the
      ``must``/``when`` extraction, which today only runs for
      ``LYS_LEAF``/``LYS_LEAFLIST`` even though ``lysc_node_container``,
      ``lysc_node_list``, and RPC/action input/output nodes carry their own
      ``musts``/``when`` too — the comment already sitting in
      ``tool_get_help()`` right after the leaf block ("Must and when
      assertions on the node itself") signals this was the intent, never
      finished.
   e. ~~Rename ``tool_get_help`` to ``tool_get_schema`` throughout:
      ``schema.c``, ``schema.h``, the ``tools[]`` entry in ``main.c``,
      ``sphinx/api.rst``, ``README.md``, ``tests/test_schema.py``. No
      backward-compatible alias: the server has no stable client base yet,
      and the new contract subsumes the old single-node one (``max_depth``
      omitted with an ``xpath`` naming a leaf behaves like today's
      ``get_help``).~~
   f. ~~Decide, and record here, what happens when ``xpath`` is omitted on a
      context with many large modules: cap the number of top-level modules
      walked per call, require at least one of ``xpath``/a still-supported
      module filter, or accept a possibly large response and rely on
      ``max_depth`` to bound it. Decision: accept the full response; the
      configured hard depth ceiling bounds recursion, and clients can pass
      an XPath when they need a narrower result.
   g. ~~Rewrite ``tests/test_schema.py`` for the merged contract: default
      (whole datastore) call, ``max_depth`` of 0/1/2 on a known module,
      xpath inline on nested nodes, non-leaf nodes reporting ``must``/
      ``when`` once (d) lands.
   h. ~~Update ``sphinx/api.rst`` (arguments, result shape, worked example)
      and the tool-status table in ``sphinx/architecture.rst``.~~

2. ~~**RPC input introspection**, folded into 1.d above: once ``get_schema``
   recurses into RPC/action ``input``/``output`` nodes, ``time`` in
   ``insert-food`` stops being reported as ``unknown`` on its own, without a
   separate ``get_input_schema`` tool.~~
3. ~~**No feedback after write.**~~ ``sr_edit_config`` now reports
   ``edit_nodes``: the number of explicit schema nodes in the accepted edit
   tree, including structural containers. This confirms the submitted edit
   size, not the datastore diff: repeated values still count, and removals
   caused by ``replace`` are not included. Exact before/after change reporting
   remains the scope of the planned ``sr_diff_config`` tool (P5.2).
4. ~~**Missing ``copy-config``.**~~ ``sr_copy_config`` takes required
   ``source`` and ``destination`` conventional datastore names, rejects a
   same-datastore request, and delegates the full replacement to sysrepo
   ``sr_copy_config()``. It intentionally has no per-module filter.
5. ~~**Default values: selectable output.**~~ The call chain is
   ``sr_get_config`` → ``sr_get_data()`` (without ``LYD_OPT_DEFAULT``) →
   ``tree_to_json()`` → ``lyd_print_mem()``. ``sr_get_config`` accepts an
   ``options`` string list: ``trim-defaults`` or ``all-defaults``. Omitting
   the list preserves the prior libyang output. These choices control
   printing of defaults in the returned tree; they do not request that
   sysrepo materialize implicit defaults absent from that tree.
6. ~~``sr_list_modules`` does not report enabled features.~~ The tool now
   includes enabled feature names for implemented modules and an empty list
   otherwise.

P3 — Logging and packaging
~~~~~~~~~~~~~~~~~~~~~~~~~~

1. Replace ``fprintf(stderr, ...)`` with elog: syslog, file and console
   back ends, and the elog command-line parser.
2. Honour the log level and backend selection once they are read from the
   libconfig file (see P1), instead of being declared in ``config.in`` and
   read by nobody.
3. Ship a systemd unit and an example lighttpd fragment.
4. Revisit whether ``sr_module_install`` and ``sr_module_uninstall`` can be
   allowed for identities with the right NACM permissions instead of being
   denied outright. (P0 is done; the denial was a temporary blocker.)

P4 — Tests to complete
~~~~~~~~~~~~~~~~~~~~~~

1. Run the suite in CI against a matrix of libyang and sysrepo revisions;
   the two are pinned together and bumping them is where the introspection
   code will break first.
2. Run the server under valgrind; the JSON reference counting and the
   notification queue deserve it.
3. Exercise notification replay, which needs replay support enabled on a
   module.
4. Exercise queue overflow and the ``dropped`` counter.
5. Cover ``sr_module_install`` and ``sr_module_uninstall`` beyond argument
   validation; a test that installs a module changes the shared repository
   and needs its own throwaway one.

P5 — Later / future features
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

No immediate blocker, but worth keeping on the radar.

1. Transactions spanning several tool calls, with explicit commit and
   rollback.
2. **A diff tool between two datastores.** Specified in
   :doc:`api`, ``sr_diff_config``: ``source``/``target`` datastore,
   ``xpath`` (default the datastore root) and ``max_depth`` (default 0 =
   unlimited), reading both sides with ``sr_get_data()`` and diffing them
   with libyang's ``lyd_diff_siblings()``. Implementation sub-tasks:

   a. A ``diff_to_json()`` helper turning a ``struct lyd_node`` diff tree
      into the flat ``diff`` array the spec describes — reusing
      ``tree_to_json()``'s xpath-building logic rather than duplicating it.
   b. Decide how ``max_depth`` interacts with reading each side: apply it to
      the two ``sr_get_data()`` calls before diffing (cheaper, but a node
      only different below the cutoff is invisible), or diff first and
      truncate the reported paths afterwards (correct, more expensive). The
      spec currently assumes the former; revisit if it proves misleading in
      practice.
   c. Surface libyang's ``yang:key``/``yang:value``/``yang:position``
      diff metadata for moved entries in user-ordered lists and leaf-lists,
      as ``previous_position`` — the one part of the diff format not a
      straight transcription of ``lyd_diff_siblings()``'s own output.
   d. Reject ``source == target`` with ``-32602`` rather than returning an
      empty diff, since it is almost certainly a mistake.
   e. Tests: a leaf changed, a list entry added/removed/reordered, and the
      no-op case (empty ``diff``, ``changed: 0``).
3. Metrics export.

.. note::

   **Not** on this list: OAuth2/JWT authentication, and a dedicated dry-run
   mode. See *Not planned* below for why.

Environment d'agent
-------------------

Bilan tiré de l'utilisation pratique du serveur sysrepo-mcp comme environnement
d'exécution d'outils. Objectif : identifier ce qui rend cet environnement
rapide et facile à utiliser. Les points d'amélioration identifiés ici sont
suivis dans le *Priority backlog* ci-dessus (P0 pour l'authentification, P2
pour les manques d'ergonomie agent, P3 pour le logging).

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
  project did not use libconfig at the time. (This is being reversed — see
  *P1 — Configuration* in the Priority backlog.)
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
   most of the way there — see ``sr_diff_config`` in :doc:`api` (P5) once it
   exists, and the missing ``copy-config`` tool (P2.4) to promote a validated
   ``candidate`` afterwards. A separate dry-run tool would only be
   reconsidered if that combination proves insufficient in practice.

Contributing
------------

1. Pick the earliest incomplete item from the *Priority backlog* above.
2. Read :doc:`architecture` for the technical context.
3. Follow the existing style, and build in the container.
4. Add a test that fails without the change.
5. Update this page and the affected documentation in the same commit.
