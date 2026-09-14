.. Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

TODO and Roadmap
===============

This page lists the tasks that need to be completed to make sysrepo-mcp
fully functional. The project is currently a **skeleton** with the build
system configured but no actual MCP or sysrepo functionality implemented.

.. note::

   This is a **living document** that tracks the implementation progress.
   Contributions are welcome!

Core Implementation Tasks
-------------------------

High Priority (Must Have)
~~~~~~~~~~~~~~~~~~~~~~~~

- [ ] **FastCGI Server Skeleton**: Implement basic FastCGI request/response handling
  - Accept FastCGI connections
  - Parse HTTP headers and body
  - Return proper HTTP responses
  - Handle JSON-RPC 2.0 messages

- [ ] **JSON-RPC Parser**: Implement JSON-RPC 2.0 message parsing and validation
  - Parse incoming JSON-RPC requests
  - Validate message structure
  - Generate proper JSON-RPC responses
  - Handle errors according to JSON-RPC specification

- [ ] **Session Management**: Implement MCP session handling
  - Create and manage session IDs
  - Track active sessions
  - Implement session timeout
  - Enforce maximum concurrent sessions

- [ ] **sysrepo Integration**: Connect to sysrepo library
  - Initialize sysrepo connection (``sr_conn_open()``)
  - Create sysrepo sessions (``sr_session_create()``)
  - Implement connection error handling
  - Configure sysrepo timeout and username

- [ ] **Basic Tools Implementation**:
  - [ ] ``get_status`` - Server status and health check
  - [ ] ``get_tree`` - YANG schema tree explorer
  - [ ] ``get_help`` - YANG node documentation

Medium Priority (Core Functionality)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- [ ] **Configuration Tools**:
  - [ ] ``sr_get_config`` - Read configuration from datastore
  - [ ] ``sr_edit_config`` - Modify configuration
  - [ ] ``sr_copy_config`` - Copy between datastores
  - [ ] ``sr_delete_config`` - Delete configuration

- [ ] **Operational Data Tools**:
  - [ ] ``sr_get_operational`` - Read operational state

- [ ] **Module Management**:
  - [ ] ``sr_module_install`` - Install YANG modules
  - [ ] ``sr_module_uninstall`` - Uninstall YANG modules
  - [ ] ``sr_list_modules`` - List installed modules

- [ ] **RPC/Actions**:
  - [ ] ``sr_execute_rpc`` - Execute NETCONF RPCs
  - [ ] ``sr_action`` - Execute YANG actions

- [ ] **Authentication**:
  - [ ] API key validation
  - [ ] Bearer token authentication
  - [ ] Session-based authentication

- [ ] **NACM Integration**:
  - [ ] Configure NACM users and permissions
  - [ ] Enforce access control on all operations
  - [ ] Map API keys to NACM users

Low Priority (Nice to Have)
~~~~~~~~~~~~~~~~~~~~~~~~~~~

- [ ] **Advanced Features**:
  - [ ] Bulk configuration operations
  - [ ] Transaction support (commit/rollback)
  - [ ] Configuration validation before apply
  - [ ] Configuration diff/compare

- [ ] **Subscriptions** (Limited by FastCGI):
  - [ ] ``sr_subscribe_oper_changes`` - Operational change notifications (may have limitations)
  - [ ] ``sr_subscribe_notifs`` - Event notifications (may have limitations)

- [ ] **Performance Optimizations**:
  - [ ] Connection pooling for sysrepo sessions
  - [ ] Caching for frequently accessed data
  - [ ] Batch operations support

- [ ] **Enhanced Error Handling**:
  - [ ] Detailed error messages with context
  - [ ] Error recovery mechanisms
  - [ ] Comprehensive logging

Build System Tasks
------------------

- [ ] **Docker Image**:
  - [x] Dockerfile with all dependencies
  - [x] Makefile for Docker build/run/test
  - [x] extern/ source download mechanism
  - [ ] CI/CD pipeline configuration

- [ ] **Build Configuration**:
  - [x] Kconfig (config.in) with default options
  - [ ] Makefile for main project build
  - [ ] ebuild.mk integration

- [ ] **Testing**:
  - [ ] Unit tests for each MCP tool
  - [ ] Integration tests with sysrepo
  - [ ] FastCGI transport tests
  - [ ] Error handling tests

Documentation Tasks
------------------

- [x] README.md - User documentation
- [x] docker/README.md - Docker build documentation
- [x] sphinx/architecture.rst - Architecture documentation
- [x] sphinx/install.rst - Installation guide
- [x] sphinx/api.rst - API reference
- [x] sphinx/license.rst - License information
- [x] sphinx/todo.rst - This roadmap
- [ ] Generate HTML documentation via Sphinx
- [ ] Add man pages
- [ ] Add usage examples

Not Planned (See Architecture)
-----------------------------

The following features **will NOT be implemented** due to architectural constraints:

- [X] **SSE (Server-Sent Events)**: Incompatible with FastCGI protocol
- [X] **WebSocket Transport**: Incompatible with FastCGI protocol
- [X] **Direct TCP/Unix Socket**: FastCGI-only transport via reverse proxy
- [X] **libconfig Support**: Not used in this project

Future Considerations
--------------------

Features that may be considered after core implementation is complete:

- **Additional Authentication Methods**:
  - OAuth2
  - JWT tokens
  - Certificate-based authentication

- **Encryption**:
  - End-to-end encryption for MCP messages
  - TLS 1.3 support

- **Scaling**:
  - Horizontal scaling with multiple server instances
  - Session affinity for load balancing
  - Connection pooling

- **Monitoring**:
  - Prometheus metrics export
  - Health check endpoints
  - Performance metrics

Contributing
-----------

If you wish to contribute to sysrepo-mcp, please:

1. Review this TODO list for open tasks
2. Check the architecture documentation for technical details
3. Ensure all code follows the existing style
4. Add tests for any new functionality
5. Update documentation as needed

For questions or discussions, please refer to the project's main repository.
