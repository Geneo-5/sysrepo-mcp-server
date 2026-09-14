.. Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

API Reference
=============

> **Note**: This project is a integration skeleton. The API described below
> is **planned but not yet implemented**. This documentation describes the
> target API that will be exposed by sysrepo-mcp.

This section documents the MCP tools (JSON-RPC methods) that sysrepo-mcp will
expose to AI agents. All communication uses JSON-RPC 2.0 over HTTP, forwarded
via FastCGI.

MCP Tools Overview
-----------------

sysrepo-mcp will expose the following categories of MCP tools:

1. **Configuration Operations** - Read and modify NETCONF datastore
2. **Operational Data** - Read runtime/operational state
3. **Module Management** - Install/uninstall YANG modules
4. **RPC/Actions** - Execute YANG RPCs and actions
5. **System Tools** - Server status and YANG schema exploration

All tools accept JSON-RPC 2.0 requests and return JSON-RPC 2.0 responses.

Configuration Tools
-------------------

Configuration tools allow reading and modifying the NETCONF datastore.

sr_get_config
~~~~~~~~~~~~~~

Read configuration from a YANG module.

**Request (get API keys from sysrepo-mcp module from yang/sysrepo-mcp.yang):**

.. code-block:: json

   // Get all API keys
   {
       "jsonrpc": "2.0",
       "id": 1,
       "method": "tools/call",
       "params": {
           "name": "sr_get_config",
           "arguments": {
               "xpath": "/sysrepo-mcp:api-key",
               "datastore": "running"
           }
       }
   }

**Request (get specific API key):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 2,
       "method": "tools/call",
       "params": {
           "name": "sr_get_config",
           "arguments": {
               "xpath": "/sysrepo-mcp:api-key[key='abc123']",
               "datastore": "running"
           }
       }
   }

**Request (using oven module from extern/sysrepo/examples/plugin/oven.yang):**

.. code-block:: json

   // Get full oven configuration
   {
       "jsonrpc": "2.0",
       "id": 3,
       "method": "tools/call",
       "params": {
           "name": "sr_get_config",
           "arguments": {
               "xpath": "/oven:oven",
               "datastore": "running"
           }
       }
   }

**Parameters:**

- ``xpath`` (string, required): XPath expression to select data nodes (must include module namespace prefix, e.g., "/sysrepo-mcp:api-key" for API keys from ``yang/sysrepo-mcp.yang``, or "/oven:oven" for the oven module)
- ``datastore`` (string, optional): Datastore to read from. Values: "running", "startup", "candidate". Default: "running"
- ``depth`` (string, optional): Traversal depth. Values: "shallow", "deep", "children". Default: "deep"

**Returns:**

- ``data`` (object): Configuration data in libyang LYD_JSON format (compatible with `lyd_print_fd` with `LYD_JSON`). Contains the YANG module data with namespace-prefixed keys (e.g., `{"sysrepo-mcp:api-key": [{"key": "abc123", "user": "admin"}]}` for API keys from ``yang/sysrepo-mcp.yang``)
- ``module`` (string): YANG module name
- ``xpath`` (string): XPath of the returned data

**Example Response (API keys from sysrepo-mcp module - libyang LYD_JSON format):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 2,
       "result": {
           "data": {
               "sysrepo-mcp:api-key": [
                       {
                           "key": "abc123",
                           "user": "admin"
                       },
                       {
                           "key": "def456",
                           "user": "operator"
                       }
                   ]
           }
       }
   }

**Example Response (oven configuration - libyang LYD_JSON format):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 3,
       "result": {
           "data": {
               "oven:oven": {
                   "turned-on": false,
                   "temperature": 0
               }
           }
       }
   }

**Example Response (oven temperature only - libyang LYD_JSON format):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 4,
       "result": {
           "data": {
               "oven:oven": {
                   "temperature": 180
               }
           }
       }
   }

sr_edit_config
~~~~~~~~~~~~~~~

Apply configuration changes to a YANG module.

**Request (add an API key with NACM user from yang/sysrepo-mcp.yang):**

.. code-block:: json

   // Add API key "abc123" for NACM user "admin"
   {
       "jsonrpc": "2.0",
       "id": 3,
       "method": "tools/call",
       "params": {
           "name": "sr_edit_config",
           "arguments": {
               "target": "running",
               "config": {
                   "sysrepo-mcp:api-key": [
                           {
                               "key": "abc123",
                               "user": "admin"
                           }
                       ]
               },
               "xpath": "/sysrepo-mcp:api-key"
           }
       }
   }

**Example with the oven YANG model (from extern/sysrepo/examples/plugin/oven.yang):**

.. code-block:: json

   // Set oven temperature to 200 degrees and turn it on
   {
       "jsonrpc": "2.0",
       "id": 3,
       "method": "tools/call",
       "params": {
           "name": "sr_edit_config",
           "arguments": {
               "target": "running",
               "config": {
                   "oven:oven": {
                       "temperature": 200,
                       "turned-on": true
                   }
               },
               "xpath": "/oven:oven"
           }
       }
   }

   // Update only the temperature (partial update)
   {
       "jsonrpc": "2.0",
       "id": 4,
       "method": "tools/call",
       "params": {
           "name": "sr_edit_config",
           "arguments": {
               "target": "running",
               "config": {
                   "oven:oven": {
                       "temperature": 150
                   }
               },
               "xpath": "/oven:oven/temperature"
           }
       }
   }

**Parameters:**

- ``target`` (string, required): Target datastore. Values: "running", "startup", "candidate"
- ``config`` (object, required): Configuration data in JSON format (compatible with libyang/sysrepo). Must match the YANG schema structure with module namespace prefixes as JSON keys (e.g., `{"sysrepo-mcp:api-key": [{"key": "abc123", "user": "admin"}]}` for API keys from ``yang/sysrepo-mcp.yang``, or `{"oven:oven": {"temperature": 180}}` for the oven module)
- ``xpath`` (string, required): XPath expression to target specific nodes (must include module namespace prefix, e.g., "/sysrepo-mcp:api-key" or "/oven:oven")

**Returns:**

- ``ok`` (boolean): true if the edit was successful
- ``error`` (string, optional): Error message if the edit failed

Operational Data Tools
----------------------

Operational tools allow reading runtime and operational state data.

sr_get_operational
~~~~~~~~~~~~~~~~~~~

Read operational state data from the datastore, including server operational state from the ``sysrepo-mcp`` YANG module.

**Request (server operational state from sysrepo-mcp module):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 3,
       "method": "tools/call",
       "params": {
           "name": "sr_get_operational",
           "arguments": {
               "xpath": "/sysrepo-mcp:server-state"
           }
       }
   }

**Request (using oven module from extern/sysrepo/examples/plugin/oven.yang):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 4,
       "method": "tools/call",
       "params": {
           "name": "sr_get_operational",
           "arguments": {
               "xpath": "/oven:oven-state"
           }
       }
   }

**Parameters:**

- ``xpath`` (string, required): XPath expression selecting the data to retrieve (e.g., "/sysrepo-mcp:server-state" for server state from ``yang/sysrepo-mcp.yang``, "/oven:oven-state" for oven state)
- ``datastore`` (string, optional): Datastore to read from. Default: "operational"
- ``depth`` (string, optional): Traversal depth. Values: "shallow", "deep", "children"

**Returns:**

- ``data`` (object): Operational state data in libyang LYD_JSON format (e.g., `{"sysrepo-mcp:server-state": {"version": "0.1.0", "uptime-seconds": 3600, "active-sessions": 0, "max-sessions": 64}}` for server state from ``yang/sysrepo-mcp.yang``)

**Example Response (server state from sysrepo-mcp module - libyang LYD_JSON format):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 3,
       "result": {
           "data": {
               "sysrepo-mcp:server-state": {
                   "version": "0.1.0-skeleton",
                   "uptime-seconds": 3600,
                   "active-sessions": 0,
                   "max-sessions": 64
               }
           }
       }
   }

**Example Response (oven state - libyang LYD_JSON format):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 4,
       "result": {
           "data": {
               "oven:oven-state": {
                   "temperature": 180,
                   "food-inside": true
               }
           }
       }
   }

Module Management Tools
-----------------------

Module management tools allow installing and uninstalling YANG modules.

sr_module_install
~~~~~~~~~~~~~~~~~~

Install a YANG module into the datastore.

**Request:**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 4,
       "method": "tools/call",
       "params": {
           "name": "sr_module_install",
           "arguments": {
               "yang_file": "/usr/share/yang/modules/ietf-interfaces@2018-02-20.yang",
               "features": "if-mib",
               "imports": "/usr/share/yang/modules"
           }
       }
   }

**Parameters:**

- ``yang_file`` (string, required): Path to the YANG schema file
- ``features`` (string, optional): Comma-separated list of features to enable
- ``imports`` (string, optional): Comma-separated list of import paths

**Returns:**

- ``module_name`` (string): Name of the installed module
- ``revision`` (string): Revision date of the installed module

sr_module_uninstall
~~~~~~~~~~~~~~~~~~~~

Uninstall a YANG module from the datastore.

**Request:**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 5,
       "method": "tools/call",
       "params": {
           "name": "sr_module_uninstall",
           "arguments": {
               "module_name": "ietf-interfaces"
           }
       }
   }

**Parameters:**

- ``module_name`` (string, required): Name of the YANG module to remove

**Returns:**

- ``removed`` (boolean): true if the module was successfully uninstalled

RPC and Action Tools
-------------------

RPC and action tools allow executing YANG-defined operations.

sr_execute_rpc
~~~~~~~~~~~~~~~

Execute a raw NETCONF RPC operation.

**Request:**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 6,
       "method": "tools/call",
       "params": {
           "name": "sr_execute_rpc",
           "arguments": {
               "xpath": "/ietf-netconf:get-config",
               "input_params": {
                   "source": "running"
               }
           }
       }
   }

**Parameters:**

- ``xpath`` (string, required): Full XPath to the RPC operation (format: ``/module:rpc-name``, e.g., ``/ietf-netconf:get-config`` for standard NETCONF RPCs from ``extern/sysrepo/examples/plugin/oven.yang`` or custom YANG modules)
- ``input_params`` (object, optional): RPC input parameters as key-value pairs

**Returns:**

- ``output`` (object): RPC output data

sr_action
~~~~~~~~

Execute a YANG action (RPC-style operation defined in a YANG model).

**Request:**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 7,
       "method": "tools/call",
       "params": {
           "name": "sr_action",
           "arguments": {
               "xpath": "/my-module:reset-interface",
               "input_params": {
                   "reset-type": "hard"
               }
           }
       }
   }

**Parameters:**

- ``xpath`` (string, required): Full XPath to the YANG action (format: ``/module:action-name``, e.g., ``/my-module:reset-interface`` for actions defined in YANG modules from ``extern/sysrepo/examples/plugin/oven.yang``)
- ``input_params`` (object, optional): Action input parameters

**Returns:**

- ``output`` (object): Action output data

System Tools
-----------

System tools provide server status and YANG schema exploration.

get_status
~~~~~~~~~

Get server status and health information.

**Request:**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 8,
       "method": "tools/call",
       "params": {
           "name": "get_status",
           "arguments": {
               "verbose": false
           }
       }
   }

**Parameters:**

- ``verbose`` (boolean, optional): Include detailed session information. Default: false

**Returns:**

- ``version`` (string): Server version (e.g., "0.1.0-skeleton")
- ``uptime_seconds`` (integer): Server uptime in seconds
- ``active_sessions`` (integer): Number of active MCP sessions
- ``max_sessions`` (integer): Maximum concurrent sessions configured
- ``session_ids`` (array, optional): List of active session IDs (only if verbose=true)

**Example Response:**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 8,
       "result": {
           "version": "0.1.0-skeleton",
           "uptime_seconds": 3600,
           "active_sessions": 0,
           "max_sessions": 64
       }
   }

**Note**: For server operational state data (e.g., from sysrepo-mcp YANG module), use ``sr_get_operational``.

get_tree
~~~~~~~

Get the YANG schema tree for a module (using ``yang/sysrepo-mcp.yang`` or ``extern/sysrepo/examples/plugin/oven.yang``).

**Request (get full sysrepo-mcp schema tree):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 9,
       "method": "tools/call",
       "params": {
           "name": "get_tree",
           "arguments": {
               "module": "sysrepo-mcp",
               "with-comments": true
           }
       }
   }

**Request (get API keys subtree from sysrepo-mcp):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 10,
       "method": "tools/call",
       "params": {
           "name": "get_tree",
           "arguments": {
               "module": "sysrepo-mcp",
               "xpath": "/sysrepo-mcp:api-key",
               "with-comments": true
           }
       }
   }

**Request (get full oven schema tree):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 11,
       "method": "tools/call",
       "params": {
           "name": "get_tree",
           "arguments": {
               "module": "oven",
               "xpath": "/",
               "with-comments": true
           }
       }
   }

**Parameters:**

- ``module`` (string, required): YANG module name (e.g., "sysrepo-mcp" for API keys and server state from ``yang/sysrepo-mcp.yang``, or "oven" for the oven example from ``extern/sysrepo/examples/plugin/oven.yang``)
- ``xpath`` (string, required): XPath expression for the sub-path within the module (e.g., "/sysrepo-mcp:api-key" for API keys, or "/oven:oven" for the full oven schema). Use "/" for the module root.
- ``revision`` (string, optional): Module revision date. Default: latest
- ``with-comments`` (boolean, optional): Include descriptions. Default: false

**Returns:**

- ``tree`` (object): Hierarchical representation of the YANG schema
- ``nodes`` (array): Flat list of all nodes with their XPath expressions
- ``references`` (array): List of module dependencies

**Example Response (oven schema tree, simplified):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 9,
       "result": {
           "tree": {
               "module": "oven",
               "namespace": "urn:sysrepo:oven",
               "prefix": "ov",
               "revision": "2018-01-19",
               "nodes": {
                   "oven:oven": {
                       "type": "container",
                       "description": "Configuration container of the oven.",
                       "children": {
                           "turned-on": {
                               "type": "leaf",
                               "leaf-type": "boolean",
                               "description": "Main switch determining whether the oven is on or off.",
                               "default": false
                           },
                           "temperature": {
                               "type": "leaf",
                               "leaf-type": "uint8",
                               "description": "Slider for configuring the desired temperature.",
                               "range": "0..250",
                               "default": 0
                           }
                       }
                   },
                   "oven:oven-state": {
                       "type": "container",
                       "config": false,
                       "description": "State data container of the oven.",
                       "children": {
                           "temperature": {
                               "type": "leaf",
                               "leaf-type": "uint8",
                               "description": "Actual temperature inside the oven."
                           },
                           "food-inside": {
                               "type": "leaf",
                               "leaf-type": "boolean",
                               "description": "Informs whether the food is inside the oven or not."
                           }
                       }
                   }
               }
           },
           "nodes": [
               {"xpath": "/oven:oven", "type": "container"},
               {"xpath": "/oven:oven/turned-on", "type": "leaf"},
               {"xpath": "/oven:oven/temperature", "type": "leaf"},
               {"xpath": "/oven:oven-state", "type": "container"},
               {"xpath": "/oven:oven-state/temperature", "type": "leaf"},
               {"xpath": "/oven:oven-state/food-inside", "type": "leaf"}
           ],
           "references": []
       }
   }

get_help
~~~~~~~

Get documentation for a specific YANG node or XPath (using ``yang/sysrepo-mcp.yang`` or ``extern/sysrepo/examples/plugin/oven.yang``).

**Request (get help for API key list from sysrepo-mcp module):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 11,
       "method": "tools/call",
       "params": {
           "name": "get_help",
           "arguments": {
               "xpath": "/sysrepo-mcp:api-key"
           }
       }
   }

**Request (get help for oven temperature leaf):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 12,
       "method": "tools/call",
       "params": {
           "name": "get_help",
           "arguments": {
               "xpath": "/oven:oven/temperature"
           }
       }
   }

**Parameters:**

- ``xpath`` (string, required): XPath expression to query (e.g., "/sysrepo-mcp:api-key" for API keys from ``yang/sysrepo-mcp.yang``, or "/oven:oven/temperature" for the oven module)
- ``module`` (string, optional): YANG module name (auto-detected if not provided)

**Returns:**

- ``xpath`` (string): The matched XPath
- ``node_type`` (string): YANG node type (leaf, container, list, etc.)
- ``description`` (string): YANG description statement
- ``mandatory`` (boolean): Whether the node is mandatory
- ``default_value`` (string, optional): Default value if specified
- ``possible_values`` (array, optional): Enum or leafref values
- ``example`` (string, optional): Example usage

**Example Response (API key list help - from sysrepo-mcp module):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 11,
       "result": {
           "xpath": "/sysrepo-mcp:api-key",
           "node_type": "list",
           "description": "List of API keys with associated NACM users.",
           "module": "sysrepo-mcp",
           "namespace": "urn:sysrepo-mcp:server"
       }
   }

**Example Response (oven temperature help):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 12,
       "result": {
           "xpath": "/oven:oven/temperature",
           "node_type": "leaf",
           "type": "oven-temperature",
           "base_type": "uint8",
           "description": "Slider for configuring the desired temperature.",
           "mandatory": false,
           "default_value": "0",
           "range": "0..250",
           "module": "oven",
           "namespace": "urn:sysrepo:oven"
       }
   }

**Example Response (oven turned-on help):**

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 13,
       "result": {
           "xpath": "/oven:oven/turned-on",
           "node_type": "leaf",
           "type": "boolean",
           "description": "Main switch determining whether the oven is on or off.",
           "mandatory": false,
           "default_value": "false",
           "module": "oven",
           "namespace": "urn:sysrepo:oven"
       }
   }

Error Handling
--------------

All tools may return errors in the following format:

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 1,
       "error": {
           "code": -32602,
           "message": "Invalid parameters",
           "data": {
               "details": "module parameter is required"
           }
       }
   }

Common error codes:

- ``-32602``: Invalid parameters
- ``-32603``: Internal error
- ``-32000``: Server error
- ``-32001``: Authentication failed
- ``-32002``: Access denied (NACM)
- ``-32003``: Module not found
- ``-32004``: Operation not supported

Transport Protocol
------------------

All MCP communication uses:

- **Protocol**: JSON-RPC 2.0
- **Transport**: HTTP POST over FastCGI
- **Content-Type**: application/json
- **Encoding**: UTF-8

The server listens on a FastCGI socket (configured via build-time options).
A reverse proxy (lighttpd, nginx) must be configured to forward HTTP requests
to the FastCGI socket.

Example lighttpd Configuration:

.. code-block:: nginx

   fastcgi.server = (
       "/mcp" => (
           "sysrepo-mcp" => (
               "socket" => "/var/run/sysrepo-mcp.sock",
               "check-local" => "disable",
               "bin-path" => "/usr/local/bin/sysrepo-mcp",
               "max-procs" => 4
           )
       )
   )
