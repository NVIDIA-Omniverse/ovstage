# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

import os
import sys

# Add the package to the path for autodoc
sys.path.insert(0, os.path.abspath("../python"))
# Add local extensions
sys.path.insert(0, os.path.abspath("_ext"))

# -- Project information -----------------------------------------------------
project = "ovstage"
copyright = "2025-2026, NVIDIA Corporation"
author = "NVIDIA Corporation"

# The version info
from ovstage import __version__

version = __version__
release = __version__

# -- General configuration ---------------------------------------------------
extensions = [
    "sphinx.ext.autodoc",
    "sphinx.ext.autosummary",
    "sphinx.ext.napoleon",
    "sphinx.ext.intersphinx",
    "sphinx_copybutton",
    "sphinx_design",
    "sphinx_mdinclude",
    "breathe",
    "filtered_literalinclude",
]

# -- Options for Breathe (C API documentation) -------------------------------
breathe_projects = {"ovstage": "_doxygen/xml"}
breathe_default_project = "ovstage"
breathe_default_members = ("members", "undoc-members")
breathe_domain_by_extension = {"h": "c"}

# -- Options for sphinx-mdinclude (Markdown in docstrings) -------------------
# Enable Markdown-to-RST conversion for docstrings
mdinclude_transform = True

# README.md is the build guide for this directory, not a page in the site.
exclude_patterns = ["_build", "Thumbs.db", ".DS_Store", "README.md"]

# Suppress warnings for known issues
suppress_warnings = [
    "autodoc.duplicate_object",  # Dataclass fields documented twice by autodoc
    "duplicate_declaration.cpp",  # Breathe generates duplicate typedef/enum declarations for C typedef patterns
    "duplicate_declaration.c",  # Same, for C domain (breathe_domain_by_extension maps .h to C)
]

# -- Options for autodoc -----------------------------------------------------
autodoc_default_options = {
    "members": True,
    "member-order": "bysource",
    "special-members": "__init__",
    "undoc-members": True,
    "exclude-members": "__weakref__",
}

autodoc_typehints = "description"
autodoc_class_signature = "separated"
# Public symbols are re-exported from ovstage/__init__.py but defined in the
# private ovstage._src.* modules. Strip the module prefix so the reference shows
# clean names (Stage, PathDictionary, ...) instead of the internal _src paths.
add_module_names = False

# The ovstage bindings load libovstage lazily (only when a Stage/PathDictionary
# is constructed), so importing the package for autodoc does not require the
# compiled shared library to be present. If that ever changes, add the modules
# that require the native library to autodoc_mock_imports here.
autodoc_mock_imports = []

# Public API classes are defined in the private ``ovstage._src.*`` modules and
# re-exported from ``ovstage/__init__.py``. Rebind their ``__module__`` to
# ``ovstage`` for the doc build only (in-memory; the shipped package is not
# modified) so autodoc — including cross-reference tooltips — shows the public
# ``ovstage.*`` path and never leaks the internal ``_src`` layout.
import ovstage as _ovstage

for _name in getattr(_ovstage, "__all__", []):
    _obj = getattr(_ovstage, _name, None)
    if isinstance(_obj, type) and getattr(_obj, "__module__", "").startswith("ovstage._src"):
        _obj.__module__ = "ovstage"

# The public submodules ``ovstage.population`` and ``ovstage.instancing`` are
# documented via ``.. automodule::`` and expose classes (e.g. ``Operation``) that
# are also defined under ``ovstage._src.*``. The loop above only covers the
# top-level ``ovstage.__all__``, so rebind each submodule's own ``__all__`` classes
# to the public submodule path too, otherwise objects.inv leaks e.g.
# ``ovstage._src.population.Operation``.
for _subname in ("population", "instancing"):
    _sub = getattr(_ovstage, _subname, None)
    if _sub is None:
        continue
    for _name in getattr(_sub, "__all__", ()):
        _obj = getattr(_sub, _name, None)
        if isinstance(_obj, type) and getattr(_obj, "__module__", "").startswith("ovstage._src"):
            _obj.__module__ = f"ovstage.{_subname}"

# -- Options for Napoleon (Google/NumPy docstrings) --------------------------
napoleon_google_docstring = True
napoleon_numpy_docstring = True
napoleon_include_init_with_doc = True
napoleon_include_private_with_doc = False
napoleon_include_special_with_doc = True
napoleon_use_admonition_for_examples = False
napoleon_use_admonition_for_notes = True
napoleon_use_admonition_for_references = False
napoleon_use_ivar = False
napoleon_use_param = True
napoleon_use_rtype = True
napoleon_preprocess_types = False
napoleon_type_aliases = None
napoleon_attr_annotations = True

# -- Options for HTML output -------------------------------------------------
html_theme = "nvidia_sphinx_theme"

html_theme_options = {
    "collapse_navigation": False,
    "navigation_depth": 4,
    "pygments_light_style": "sas",  # for light mode
    "pygments_dark_style": "github-dark",  # for dark mode
    "icon_links": [
        {
            "name": "GitHub",
            "url": "https://github.com/NVIDIA-Omniverse/ovstage",
            "icon": "fa-brands fa-github",
            "type": "fontawesome",
        },
    ],
}

html_static_path = ["_static"]
# Paths are relative to html_static_path
html_css_files = [
    "css/custom.css",
]

# -- Options for intersphinx -------------------------------------------------
intersphinx_mapping = {
    "python": ("https://docs.python.org/3", None),
    "numpy": ("https://numpy.org/doc/stable/", None),
}

# -- Autosummary settings ----------------------------------------------------
autosummary_generate = True


# -- Convert Markdown code blocks in docstrings to RST -----------------------
import re


def convert_markdown_codeblocks(app, what, name, obj, options, lines):
    """Convert Markdown fenced code blocks to RST code-block directives.

    Also handles Example/Examples sections with plain Python code.
    """
    text = "\n".join(lines)

    # Pattern for ```lang ... ``` code blocks
    pattern = r"```(\w*)\n(.*?)```"

    def replace_codeblock(match):
        lang = match.group(1) or "python"
        code = match.group(2).rstrip()
        # Indent the code for RST
        indented = "\n".join("    " + line if line else "" for line in code.split("\n"))
        return f".. code-block:: {lang}\n\n{indented}\n"

    text = re.sub(pattern, replace_codeblock, text, flags=re.DOTALL)

    # Handle Example/Examples sections with plain Python code
    # Pattern: "Example:" or "Examples:" followed by indented code (not >>> style)
    lines_list = text.split("\n")
    result = []
    i = 0
    in_example = False
    example_indent = 0
    code_block_lines = []

    while i < len(lines_list):
        line = lines_list[i]
        stripped = line.strip()

        # Check for Example: or Examples: section start
        if stripped in ("Example:", "Examples:"):
            in_example = True
            result.append(line)
            result.append("")  # Blank line after Example:
            result.append(".. code-block:: python")
            result.append("")
            i += 1
            # Skip blank lines after Example:
            while i < len(lines_list) and not lines_list[i].strip():
                i += 1
            # Determine indentation of first code line
            if i < len(lines_list):
                first_code = lines_list[i]
                example_indent = len(first_code) - len(first_code.lstrip())
            continue

        if in_example:
            # Check if still in example section (indented or blank)
            if not stripped:
                # Blank line - could be end or continuation
                # Look ahead to see if more code follows
                j = i + 1
                while j < len(lines_list) and not lines_list[j].strip():
                    j += 1
                if j < len(lines_list):
                    next_line = lines_list[j]
                    next_indent = len(next_line) - len(next_line.lstrip())
                    # Check if next non-blank is still code (indented). Indented
                    # lines ending in ':' (if/for/def/...) are still code, not a
                    # new docstring section.
                    if next_indent >= example_indent:
                        result.append("")  # Keep blank line in code block
                        i += 1
                        continue
                # End of example section
                in_example = False
                result.append(line)
            elif (
                (len(line) - len(line.lstrip())) < example_indent
                and stripped.endswith(":")
                and not stripped.startswith("#")
            ):
                # New docstring section (Args:, Returns:, ...) at a shallower
                # indent than the example body — not an indented code line.
                in_example = False
                result.append(line)
            else:
                # Code line - ensure proper indentation (4 spaces for RST code block)
                code_content = (
                    line[example_indent:]
                    if len(line) > example_indent
                    else line.lstrip()
                )
                result.append("    " + code_content)
        else:
            result.append(line)
        i += 1

    # Update lines in-place
    lines.clear()
    lines.extend(result)


def setup(app):
    app.connect("autodoc-process-docstring", convert_markdown_codeblocks)
