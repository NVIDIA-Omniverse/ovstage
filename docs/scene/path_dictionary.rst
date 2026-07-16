.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Path Dictionary
===============

The OVX path dictionary is the shared interning layer that maps strings —
attribute names and prim paths — to stable, trivially comparable handles used
across OV libraries. Interning a string returns an ``ovx_token_t``; a prim path
returns an ``ovx_primpath_t``; a set of paths becomes an immutable
``ovx_primpath_list_t`` that you pass to ovstage queries.

For handle-compatible sharing, obtain ovstage's own dictionary rather than
creating a separate one, so handles minted by your application and by ovstage
compare directly:

* **C** — ``ovstage_get_path_dictionary``.
* **Python** — ``ovstage.PathDictionary(stage)`` borrows the instance-owned dictionary.

Handle Types
------------

.. list-table::
   :header-rows: 1

   * - Handle
     - Interns
     - Notes
   * - ``ovx_token_t``
     - A string (e.g. an attribute name).
     - Same string always interns to the same token; compare tokens by value.
   * - ``ovx_primpath_t``
     - A single prim path.
     - Same path always interns to the same handle.
   * - ``ovx_primpath_list_t``
     - An ordered set of prim paths.
     - Immutable after creation; equal list handles imply the same paths in the same order.

The ``OVX_INVALID_*`` sentinels are all ``0`` and are never returned on success.
Because identical inputs intern to identical handles, consumers use O(1) handle
equality instead of string comparison at every boundary.

Interning and Resolving
-----------------------

Intern a string to a token, then resolve the token back to its string:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/minimal/main.py
         :language: python
         :start-after: # [snippet:intern-and-resolve]
         :end-before: # [/snippet:intern-and-resolve]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
         :language: cpp
         :start-after: // [snippet:intern-and-resolve]
         :end-before: // [/snippet:intern-and-resolve]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Building a Path List and Opening a Query
----------------------------------------

An interned prim-path list is the input to :doc:`/scene/queries`. Build one from
strings and open a query over those prims:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/minimal/main.py
         :language: python
         :start-after: # [snippet:path-list-query]
         :end-before: # [/snippet:path-list-query]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
         :language: cpp
         :start-after: // [snippet:path-list-query]
         :end-before: // [/snippet:path-list-query]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Attribute Arguments: String or Token
-------------------------------------

APIs that take an attribute accept either an already-interned token (the hot
path — no lookup) or a plain string (interned at call time). In C this is the
``ovx_string_or_token_t`` dual-mode argument; in Python you can pass an ``int``
token or a ``str``:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/minimal/main.py
         :language: python
         :start-after: # [snippet:string-or-token-arg]
         :end-before: # [/snippet:string-or-token-arg]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
         :language: cpp
         :start-after: // [snippet:string-or-token-arg]
         :end-before: // [/snippet:string-or-token-arg]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Ownership and Lifetime
----------------------

* **The dictionary is owned by its producing subsystem.** When you borrow
  ovstage's dictionary, do not free its handle; ovstage owns it.
* **Tokens and paths are dictionary-lifetime.** They are interned and are never
  freed individually.
* **Dictionary string pointers are borrowed.** In C, an ``ovx_string_t``
  returned by the dictionary points into dictionary-owned storage; copy it if it
  must outlive the dictionary (refer to :doc:`/concepts/string_handling`).
* **Path lists are explicitly refcounted.** ``create_path_list_from_*`` returns
  a list owned by the caller; pair each create with exactly one release. A path
  list handed back inside a read result is a *borrow* — do not release it.

Errors
------

Every C path-dictionary call returns ``ovx_api_result_t { status, error }``; on
``OVX_API_ERROR`` the error string must be released with
``path_dictionary_release_error``. In Python, path-dictionary failures raise
``ovstage.OvxError``. Refer to :doc:`/concepts/error_handling`.

Where to Go Next
----------------

- :doc:`/scene/queries` — resolve a path list into per-attribute prim groups.
- :doc:`/scene/writing_attributes` / :doc:`/scene/reading_attributes` — use tokens as attribute keys.
- :doc:`/concepts/string_handling` — working with ``ovx_string_t`` in C.
