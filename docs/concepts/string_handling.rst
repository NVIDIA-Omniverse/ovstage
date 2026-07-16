.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

String Handling
===============

ovstage passes strings as ``ovx_string_t``, a non-owning ``(ptr, length)`` view
that is **not required to be null-terminated**. The caller owns the pointed-to
bytes, ``length`` is a byte count (not a code-point count), and the text is
UTF-8. Many entry points — attribute names, filter attributes, prim paths —
accept the dual-mode ``ovx_string_or_token_t``, which carries either a
pre-resolved path-dictionary token or a raw string.

``ovx_string_t``
----------------

.. code-block:: c

   typedef struct ovx_string_t {
       const char* ptr;     // borrowed bytes, not necessarily null-terminated
       size_t      length;  // byte count
   } ovx_string_t;

Always use both fields together:

- **Print** with ``%.*s``, passing ``(int)length, ptr`` — never assume a
  terminator:

  .. code-block:: c

     printf("%.*s\n", (int)name.length, name.ptr);

- **Compare** by checking ``length`` first, then ``memcmp`` / ``strncmp`` over
  exactly ``length`` bytes.
- In C++, wrap a borrowed view as ``std::string_view{ s.ptr, s.length }``; copy
  to a ``std::string`` if it must outlive the source buffer:

  .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
     :language: cpp
     :start-after: // [snippet:string-view-from-ovx-string]
     :end-before: // [/snippet:string-view-from-ovx-string]
     :exclude-pattern: ^\s*//\s*\[/?snippet:
     :dedent:

A string view returned from the path dictionary is valid only for the
dictionary's lifetime — copy it to outlive the dictionary.

``ovx_string_or_token_t``: String or Token
------------------------------------------

.. code-block:: c

   typedef struct ovx_string_or_token_t {
       ovx_token_t  token;   // uint64; 0 == unresolved (use string)
       ovx_string_t string;  // resolved through the dictionary when token == 0
   } ovx_string_or_token_t;

If ``token != 0`` the token is used directly; if ``token == 0`` the ``string`` is
resolved through the path dictionary at call time. Pre-resolving a token avoids
per-call hashing in hot loops:

.. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
   :language: cpp
   :start-after: // [snippet:string-or-token-arg]
   :end-before: // [/snippet:string-or-token-arg]
   :exclude-pattern: ^\s*//\s*\[/?snippet:
   :dedent:

In Python
---------

Python has no ``ovx_string_t``. Methods accept a plain ``str`` (interned each
call) or a pre-interned ``int`` token (cheaper for repeated use):

.. tab-set::

   .. tab-item:: Intern and Resolve

      .. filtered-literalinclude:: ../../examples/python/minimal/main.py
         :language: python
         :start-after: # [snippet:intern-and-resolve]
         :end-before: # [/snippet:intern-and-resolve]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: String or Token Argument

      .. filtered-literalinclude:: ../../examples/python/minimal/main.py
         :language: python
         :start-after: # [snippet:string-or-token-arg]
         :end-before: # [/snippet:string-or-token-arg]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

Where to Go Next
----------------

- :doc:`/scene/path_dictionary` — interning, token identity, and token lifetime.
- :doc:`error_handling` — the ``ovx_string_t`` values returned by the diagnostics accessors.
