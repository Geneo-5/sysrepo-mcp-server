.. Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

Roadmap
=======

This appendix is the single source of truth for the state of the project. It
tracks what remains to be done, and in which order the remaining work should
be done.

.. note::

   Keep this page and the reality of the tree in sync. A roadmap that claims
   more than the code delivers is worse than no roadmap at all: it is what
   sends a reader looking for a feature that was never written.

Priority backlog
----------------

Only unfinished work appears here, ordered by impact. Completed work was
removed from this page; its delivered behavior is documented in the
corresponding API/architecture pages and in the git history.

P1 — Interoperability and deployment verification
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. **Expand nested unions in ``get_schema``.** ``union`` leaves report their
   member types, but a union of unions is reported as ``type: union`` without
   expanding the inner members. The test module ``sysrepo-mcp-test.yang``
   does not exercise this case yet, and no real-world model uses it: add a
   nested union to the fixture, then expand it in ``add_union_member()``.

2. **Declare every handler argument in its ``inputSchema``.** Only
   ``sr_edit_config`` has been checked
   (``test_sr_edit_config_declares_config_required``). Add a test asserting
   that every argument a handler reads is declared in its ``inputSchema``,
   for all tools, and fix any tool where the catalogue in ``src/main.c``
   drifts from the handler.

P2 — Protocol, agent and operator capabilities
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

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
3. **``previous_position`` in ``sr_diff_config``.** For user-ordered lists and
   leaf-lists (libyang's ``yang:key``/``yang:value``/``yang:position``
   metadata). The model under test (the oven) has no such list, so the tests
   cannot exercise it: add a fixture with a user-ordered list, then implement.

4. **Transactions spanning several tool calls.** Let an agent stage several
   edits in its session, then apply them together or drop them. Today
   ``sr_edit_config`` applies at once, and the only atomic multi-step path is
   ``sr_copy_config`` from ``candidate``, which replaces *every* module's
   data. The contract is in :doc:`api` (*Transactions*, status *planned*) and
   the design in :doc:`architecture`. Four new tools (``sr_txn_begin``,
   ``sr_txn_status``, ``sr_txn_commit``, ``sr_txn_rollback``) and a ``stage``
   argument on ``sr_edit_config`` and ``sr_delete_config``. They are
   session-bound, so legacy ``2025-11-25`` clients only, like the
   notification tools; revisit together with item 1 if a stateless identity
   is ever defined.

   Decisions taken (change them here before coding, not during):

   - Staged operations live in the MCP session **as data** (the ``config``
     JSON text and its ``operation``, or the XPath to delete), not as pending
     changes of a sysrepo session. sysrepo holds a CONTEXT READ LOCK from the
     first stored change until apply or discard, which would block
     ``sr_install_module()`` and every other call that needs the CONTEXT
     WRITE LOCK for as long as an agent leaves a transaction open. And the
     ``sr_discard_changes()`` that the existing handlers call after a failed
     ``sr_edit_batch()`` would wipe the edits staged before it.
   - ``sr_txn_commit`` starts one sysrepo session for the request, replays
     the operations in order (``sr_edit_batch()`` or ``sr_delete_item()``),
     then calls ``sr_apply_changes()`` once: all or nothing. On any failure
     it calls ``sr_discard_changes()``, applies nothing and keeps the
     transaction open, so the agent chooses between re-staging and
     ``sr_txn_rollback``.
   - One transaction per session, bound to one datastore (``running``,
     ``startup`` or ``candidate``), committed or rolled back as a whole: a
     single staged operation cannot be removed.
   - No isolation: ``sr_get_config`` and ``sr_diff_config`` do not see staged
     edits and no ``sr_lock()`` is taken. Reviewing a change before applying
     it stays the job of the ``candidate`` workflow.
   - A call without ``stage: true`` made while a transaction is open is
     applied at once, exactly as before.

   Tasks:

   a. ``include/sysrepo/mcp/utilities.h``: add ``MCP_ERR_NO_TXN`` (-32009)
      and ``MCP_ERR_TXN`` (-32010); the mapping to HTTP stays 200.
   b. Configuration: ``server.session.txn_max_ops`` (default 32, range
      1-1024) and ``server.session.txn_ttl`` (idle seconds, default 300,
      range 5-3600) in ``struct mcp_config``, ``mcp_config_set_defaults()``
      and ``load_session()`` of ``src/libconfig.c``, with their range macros.
      The staged payload as a whole is also capped at ``MCP_MAX_BODY``
      (1 MiB, the bound of one request).
   c. ``include/sysrepo/mcp/sessions.h``: a ``struct mcp_txn`` embedded in
      ``struct mcp_session`` (datastore, last activity, array of operations,
      each with its kind, ``operation`` string, payload and node count).
      ``session_destroy()`` frees it; ``sessions_expire()`` discards a
      transaction idle for more than ``txn_ttl``; every ``sr_txn_*`` call and
      every staging refreshes its last activity.
   d. ``src/transactions.c`` and ``include/sysrepo/mcp/transactions.h``,
      registered in ``src/ebuild.mk``: the four handlers, plus
      ``txn_stage_edit()`` and ``txn_stage_delete()`` for the two datastore
      tools. ``sr_txn_rollback`` is idempotent (no open transaction is not an
      error); ``sr_txn_status`` answers ``open: false`` rather than failing.
   e. ``src/config_tools.c``: split ``tool_sr_edit_config()`` into a strict
      parse step and an apply step, and extract the existence check of
      ``strict`` deletes into a helper, so staging and commit share them
      (``sr_delete_item()`` with ``SR_EDIT_STRICT`` does not fail on an absent
      node, see the comment in ``tool_sr_delete_config()``). With
      ``stage: true``: no session gives -32008, no open transaction -32009, a
      ``datastore`` other than the transaction's -32602, a full transaction
      -32010. The edit is parsed with ``LYD_PARSE_STRICT`` at staging time
      (unknown nodes and bad values fail at once); constraints on the
      resulting datastore, NACM and ``strict`` deletes are only evaluated at
      commit, and the error detail names the failing operation ("operation 3
      of 5").
   f. ``src/main.c``: four catalogue entries, all with ``needs_session = 1``
      (the commit needs the per-request sysrepo session bound to the NACM
      user), and the ``stage`` property in the schemas of the two datastore
      tools. ``src/transport.c``: add the four names to
      ``tool_is_session_scoped()`` so the stateless listing omits them.
   g. ``src/status.c``: ``get_status`` reports ``active_transactions`` and,
      with ``verbose``, ``transaction_operations`` for each session.
   h. Tests, in ``tests/test_transactions.py`` on the oven fixture: nothing
      visible before commit and everything after; rollback, and a second
      rollback; a commit that fails on a ``strict`` delete of an absent node
      leaves the datastore unchanged, keeps the transaction open and names
      operation 2; stage without begin (-32009), begin twice (-32010),
      datastore mismatch (-32602), no session (-32008), the fifth staging
      with ``txn_max_ops = 4`` (-32010); the four tools absent from the
      stateless ``tools/list`` and refused by a stateless ``tools/call``; a
      non-staged edit applied at once during a transaction; expiry with
      ``txn_ttl = 5`` (``status`` then ``open: false``, ``commit`` -32009);
      ``DELETE`` of the session discards it. In ``tests/test_auth.py``: a
      ``viewer`` stages, then commit is denied (-32003) and nothing is
      applied. Adjust the catalogue assertions of ``tests/test_protocol.py``;
      the generic ``inputSchema`` test of P1.2 covers the new schemas.
      ``docker/sysrepo-mcp.conf`` (the test configuration) gets
      ``txn_max_ops = 4`` and ``txn_ttl = 5``.
   i. Documentation, in the same commit: :doc:`api` (``planned`` becomes
      ``implemented``, drop the planned notes), :doc:`architecture`,
      :doc:`install` (both settings, in *Session and schema settings*),
      ``README.md`` (feature list), ``AGENTS.md`` (state table, and the
      CONTEXT READ LOCK pitfall under *API sysrepo*). Then delete this item.

5. **Metrics export.** Counters an operator can scrape, in the Prometheus
   text format, answered by the FastCGI responder on a ``GET`` path: a
   monitoring system cannot call MCP tools, and HTTP, TLS and rate limiting
   stay with the proxy (see *Not planned*). It is not an MCP tool, so an
   agent learns nothing beyond ``get_status``. The contract, with the full
   list of metrics, is in :doc:`api` (*Metrics endpoint*, status *planned*).

   Decisions taken (change them here before coding, not during):

   - Counting is always on (an integer increment per event, no locking: the
     server is single-threaded, notification callbacks included, because of
     ``SR_SUBSCR_NO_THREAD``); only the endpoint is optional, off by default
     (``server.metrics.enabled``).
   - ``server.metrics.path`` (default ``/mcp/metrics``) is the request path as
     the proxy forwards it, compared with ``REQUEST_URI`` minus its query
     string. It must lie under the prefix already routed to the FastCGI
     socket: a second ``fastcgi.server`` entry would start a second process,
     and ``max-procs`` must stay 1.
   - With authentication on, a valid API key is required (HTTP 401
     otherwise) and no NACM rule is evaluated: any valid key reads the
     metrics.
   - Labels come from closed sets only: HTTP method and status, tool name
     (the catalogue), JSON-RPC code (the ``MCP_ERR_*`` values plus
     ``other``), event names. Never an XPath, a user, a key or a session
     identifier.
   - Endpoint disabled: a ``GET`` keeps answering 405 with
     ``Allow: POST, DELETE``, as today.

   Tasks:

   a. ``include/sysrepo/mcp/metrics.h`` and ``src/metrics.c``, registered in
      ``src/ebuild.mk``: unsigned 64-bit counters, per-tool arrays sized by
      ``TOOL_COUNT``, a fixed-bucket duration histogram, one ``metrics_*()``
      function per event, and ``metrics_render()`` producing the text body.
   b. Instrumentation: ``http_send()`` and the direct ``204`` written by
      ``serve()`` for ``DELETE`` (leave it alone: a 204 must carry no
      ``Content-Length``, so count it explicitly); ``method_tools_call()``
      around ``desc->handler()`` for calls, errors and duration;
      ``session_create()``, ``sessions_expire()``, the ``DELETE`` branch and
      the 503 branch of ``method_initialize()`` for session events;
      ``session_push_notif()`` for received and dropped notifications (the
      per-session totals vanish with the session, so the global counter is
      needed). If item 4 has landed, count its events too.
   c. ``src/transport.c``: route ``GET`` on the metrics path in ``serve()``,
      after the ``Origin`` check and the credential extraction and before
      the session logic; call ``housekeeping()`` first so the gauges are
      current; give ``http_send()`` a content-type parameter (the body is
      ``text/plain; version=0.0.4; charset=utf-8``).
   d. Configuration: ``server.metrics.enabled`` (boolean) and
      ``server.metrics.path`` (string, starts with ``/``, 1-127 bytes, no
      ``?`` or ``#``) in ``struct mcp_config``, ``mcp_config_set_defaults()``
      and a new ``load_metrics()`` in ``src/libconfig.c``; an invalid value
      fails closed like the others.
   e. Check under lighttpd which FastCGI parameter carries the path
      (``REQUEST_URI``, ``SCRIPT_NAME`` or ``PATH_INFO``) before relying on
      it: ``docker/lighttpd.conf`` and the test template in
      ``tests/conftest.py`` are the place to look.
   f. Tests, in ``tests/test_metrics.py``: 200 with the right ``Content-Type``
      and a body every line of which is a comment or a ``name{labels} value``
      sample (a minimal parser in the test, no new dependency); one
      ``get_status`` call raises ``sysrepo_mcp_tool_calls_total`` by exactly
      1 and the histogram ``_count`` with it, with cumulative buckets; a
      refused call raises ``sysrepo_mcp_tool_errors_total`` with its code;
      ``sysrepo_mcp_sessions_active`` matches ``get_status``; closing a
      session raises the ``deleted`` event; the overflow test of
      ``tests/test_sessions.py`` raises ``dropped``; ``POST`` on the path is
      405 with ``Allow: GET``; with authentication (``mcp_auth``): 401
      without a key, 401 with a wrong key, 200 with a valid one; disabled
      (a fixture modelled on ``fail_closed``, with no ``metrics`` group): 405
      with ``Allow: POST, DELETE``.
   g. ``docker/sysrepo-mcp.conf`` and ``docker/sysrepo-mcp-auth.conf``:
      ``metrics`` group enabled for the tests.
   h. Documentation, in the same commit: :doc:`api`, :doc:`architecture`
      (*Observability*), :doc:`install` (a *Metrics* subsection),
      ``contrib/lighttpd/sysrepo-mcp.conf`` (comment: the path lies under
      the ``/mcp`` prefix, so no second backend; restrict it to the scraper
      with a ``mod_access`` rule), ``README.md`` and ``AGENTS.md``. Then
      delete this item.

Not planned: OAuth2/JWT, a dedicated dry-run tool, SSE, WebSocket, or a direct
HTTP listener. See *Not planned* below for the recorded decisions.

Source layout
-------------

The code is split into one source file per functional area, each with its own
header.

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

``src/diff.c``
   ``sr_diff_config``: reads both datastores, diffs them with
   ``lyd_diff_siblings()`` and serialises the result with ``diff_to_json()``.

``src/log.c``
   ``mcp_log_*`` helpers on top of ``elog`` (syslog, file and console back
   ends), configured from the libconfig file.

Headers live under ``include/sysrepo/mcp/`` — one per source file, forward
declarations only. Every module includes ``utilities.h`` for shared helpers.

Testing
-------

The suite in ``tests/`` drives the server the way a client does: HTTP to
lighttpd on port 80, forwarded over FastCGI, against the upstream oven plugin
from ``extern/sysrepo/examples/plugin/oven.c``. It runs in a private sysrepo
repository and shared-memory namespace, so it never touches the system
repository.

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

``tests/test_auth.py``
   Authentication and NACM through a dedicated server with API keys: legacy
   and stateless credential checks, the ``-32003`` denial code, blocked
   module installation, NACM-denied RPCs, constant-time key lookup and
   fail-closed startup on an invalid configuration.

``tests/test_diff.py``
   ``sr_diff_config``: no-op, created, deleted, replaced and default-value
   cases.

``tests/test_standalone.py``
   The standalone FastCGI listeners (Unix socket and TCP), driven without a
   web-server supervisor.

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
   environment it sits next to. The libconfig API-key list plus NACM is the
   deliberately lighter mechanism this project uses instead, and that
   trade-off is not expected to change.

**Hashing API keys**
   Keys remain plaintext in the config file and process memory; SHA-256 was
   rejected as unnecessary CPU cost. Require high-entropy values,
   restrictive config-file permissions, and never log credentials.

**A dedicated dry-run tool**
   sysrepo itself has no dry-run operation to wrap. Writing to ``candidate``,
   validating it, and comparing it against ``running`` already gets an agent
   most of the way there — see ``sr_diff_config`` in :doc:`api`.
   ``sr_copy_config`` can promote a validated ``candidate``. A
   separate dry-run tool would only be
   reconsidered if that combination proves insufficient in practice.

Contributing
------------

1. Pick the earliest incomplete item from the *Priority backlog* above.
2. Read :doc:`architecture` for the technical context.
3. Follow the existing style, and build in the container.
4. Add a test that fails without the change.
5. Update this page and the affected documentation in the same commit.
