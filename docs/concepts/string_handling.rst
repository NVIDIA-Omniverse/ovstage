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
=======================

Python accepts a ``str`` or integer :term:`token <Token>` for scalar dual-mode
attribute-name arguments, such as write, map, delete, and attribute-floor lookup.
The list arguments ``Stage.read_attributes(attrs)``, ``Stage.query(attrs=...)``,
and ``Stage.advance_write_floor(attributes=...)`` require integer tokens.
Use :meth:`PathDictionary.intern_token() <ovstage.PathDictionary.intern_token>`
for those lists, and reuse tokens when passing the same names repeatedly.

In C, :c:type:`ovx_string_t` contains a pointer and a byte length. The text is
UTF-8. An input string need not end with a null character; use its length when
reading it. The view does not own its buffer.

Passing Strings or Tokens
---------------------------------

In C, arguments of type :c:type:`ovx_string_or_token_t` accept a string or a
:term:`token <Token>`. A nonzero ``token`` is used directly; otherwise the name in
``string`` is resolved through the path dictionary. Reuse a token when passing
the same name repeatedly.

Python accepts a ``str`` or an integer token for these arguments. The snippets
below show an attribute passed as a string in Python and as a token in C.
The Python snippet assumes an existing stage and query; the C snippet uses the
previously resolved ``attr`` token.

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

Use the stage's dictionary to create tokens. See :doc:`/scene/path_dictionary`
for the Python and C examples and token lifetimes.

Reading Returned Strings in C
----------------------------------------

* Read ``length`` bytes. A null pointer with zero length represents no value;
  a null pointer with nonzero length is invalid.
* Compare lengths first, then compare that many bytes with ``memcmp()``.
  For zero-length strings, no byte comparison is needed. ``strncmp()`` stops
  at a null character and is not a general byte comparison.
* In C++, use ``std::string_view`` to borrow a string or ``std::string`` to copy it.
* A nonempty library-produced ``ovx_string_t`` has a trailing null character,
  as specified by its header. This does not extend the buffer's lifetime.

In C, write the specified bytes without requiring a trailing null character.
For a valid ``name`` view, this also avoids narrowing its length to ``int``.
Include ``<stdio.h>`` and check the returned byte count for output errors:

.. code-block:: c

   // An empty view may have a null pointer.
   if (name.length != 0)
       fwrite(name.ptr, 1, name.length, stdout);

The C++ snippet below borrows the returned string using its pointer and length:

.. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
   :language: cpp
   :start-after: // [snippet:string-view-from-ovx-string]
   :end-before: // [/snippet:string-view-from-ovx-string]
   :exclude-pattern: ^\s*//\s*\[/?snippet:
   :dedent:

Keep the owner alive while using a borrowed string. Dictionary strings last for
the dictionary's lifetime. Operation error messages have a shorter lifetime;
see :doc:`error_handling`. Copy a string if you need it after its owner releases
or replaces the buffer.
