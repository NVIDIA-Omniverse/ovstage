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

The :class:`ovstage.PathDictionary` maps strings and prim paths to reusable
identifiers. Registering a string this way is called interning. Use the
stage's dictionary so the identifiers your application creates match those
used by ovstage.

Create :class:`PathDictionary(stage) <ovstage.PathDictionary>` to borrow the instance-owned dictionary.
In C, obtain it with :c:func:`ovstage_get_path_dictionary()`.

Names, Paths, and Lists
-----------------------

.. list-table::
   :header-rows: 1

   * - Python method
     - Result
   * - :meth:`intern_token(name) <ovstage.PathDictionary.intern_token>`
     - A :class:`ovstage.Token` for a string, such as an attribute name.
   * - :meth:`intern_path(path) <ovstage.PathDictionary.intern_path>`
     - A :class:`ovstage.PrimPath` for a :term:`prim <Prim>` path.
   * - :meth:`create_path_list_from_strings(paths) <ovstage.PathDictionary.create_path_list_from_strings>`
     - An immutable, ordered :class:`ovstage.PathList` for a query.

Within one dictionary, interning the same string or prim path returns the same
identifier. Compare those identifiers directly rather than resolving their strings.
Path lists are different: creating the same ordered list twice can return different
handles. Do not compare path-list handles to determine whether their contents match.
The C types are ``ovx_token_t``, ``ovx_primpath_t``, and ``ovx_primpath_list_t``
respectively.

These are distinct structs containing one ``uint64_t raw_id`` in C, with
same-kind comparison and hashing in C++. Python uses distinct handle classes,
not integer subclasses. APIs reject another handle kind or a bare integer.
In C/C++, keep handles typed in scalars, arrays, and containers. A DLPack tensor
can point directly at typed handle storage while describing it as uint64.
Read uint64 tensor payloads as numeric IDs, applying ``tensor.byte_offset`` in
bytes before accessing the elements, then construct the corresponding handle
(for example, ``ovx_token_t token{values[i]}`` in C++).
Use ``handle.raw_id`` when an interface requires an integer,
such as a Python NumPy uint64 array or numeric diagnostic output. Explicitly
construct the correct handle kind when reading such integers back. Equal numeric
values across kinds do not imply identity.
All native producers, consumers, and Python bindings must be rebuilt together.

``PathList`` extends ``PrimPathList`` with ownership. Copying it or constructing
``PrimPathList(owned_list)`` produces a non-owning typed handle, without adding a
native reference. Use ``add_path_list_reference`` when an extra reference is needed.

The empty string is a valid token and always interns to ID ``0``. Resolving
that ID returns an empty string (a non-null pointer with length zero in C).
``OVX_INVALID_TOKEN`` is ``UINT64_MAX`` and resolving it returns ``OVX_API_ERROR``
(``OvxError`` in Python). Empty prim-path strings remain invalid.
``create_paths_from_tokens`` rejects token ID ``0`` as a path component.
``OVX_INVALID_PRIMPATH`` and ``OVX_INVALID_PRIMPATH_LIST`` remain ``0``.
No successful creation call returns the corresponding invalid sentinel.

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

Constructing Prim Paths
-----------------------

In Python, use :meth:`intern_path() <ovstage.PathDictionary.intern_path>` with
an absolute path such as ``/World/Box``. Use ``/`` for the root.

In C, :c:func:`path_dictionary_create_paths_from_tokens()` also accepts component
names: tokens for ``World`` and ``Box`` produce ``/World/Box``. Do not include
separators in the names. Within one dictionary, this returns the same handle as
:c:func:`path_dictionary_create_paths_from_strings()`, regardless of call order.
Python exposes only string-based path construction.

For the root in C, set its ``num_tokens_per_path`` entry to zero. Supply a
non-null ``tokens_per_path`` array; no component tokens are read for that entry.
:c:func:`path_dictionary_get_tokens_from_paths()` returns zero components for
``/``.

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
------------------------------------

Attribute arguments accept a string or an :class:`ovstage.Token`. Reusing an interned
token avoids a string lookup on each call. In C, ``ovx_token_or_string_t`` holds
either form. These snippets show both:

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

* Each stage owns its dictionary. Python requires :class:`PathDictionary(stage) <ovstage.PathDictionary>`;
  the wrapper does not own the stage. Do not destroy the native dictionary separately.
* Path lists belong to that stage. To use the same selection on another stage,
  recreate the list in its dictionary. Release list references before destroying
  their stage; neither a list reference nor another live stage keeps them valid.
* Tokens and individual path identifiers last for the dictionary's lifetime;
  do not release them individually.
* :meth:`create_path_list() <ovstage.PathDictionary.create_path_list>` and :meth:`create_path_list_from_strings() <ovstage.PathDictionary.create_path_list_from_strings>` return an owned
  :class:`ovstage.PathList`. Use a ``with`` block or call :meth:`release() <ovstage.PathList.release>`. The object
  is a ``PrimPathList`` and can be passed wherever a list handle is accepted.
  Reuse a list outside a per-frame loop when its paths do not change.
* Lists borrowed from read results are non-owning :class:`ovstage.PrimPathList` handles. Do not release them.
* An unreleased Python ``PathList`` releases its reference on garbage collection
  and emits ``ResourceWarning``. Explicit release avoids relying on that cleanup.

In C, release each created path-list reference through the dictionary exactly
once. Returned ``ovx_string_t`` values borrow dictionary storage; copy any text
that must outlive the dictionary. See :doc:`/concepts/string_handling`.

Errors
------

Python dictionary failures raise :class:`ovstage.OvxError`. Invalid, unknown,
or expired path identifiers fail rather than resolving to an empty string.
:meth:`ovstage.PathDictionary.path_to_string()` returns ``/`` for the valid
absolute root.

In C, dictionary calls return ``ovx_api_result_t``. On ``OVX_API_ERROR``, release
the error string with :c:func:`path_dictionary_release_error() <path_dictionary_release_error>`.
:c:func:`path_dictionary_get_tokens_from_paths() <path_dictionary_get_tokens_from_paths>` returns component tokens, not a joined
path string. The root succeeds as one processed path with zero tokens; callers
join the components themselves. See :doc:`/concepts/error_handling`.

Further Reading
---------------

- :doc:`/scene/queries` — resolve a path list into per-attribute prim groups.
- :doc:`/scene/writing_attributes` / :doc:`/scene/reading_attributes` — use tokens as attribute keys.
- :doc:`/concepts/string_handling` — working with ``ovx_string_t`` in C.
