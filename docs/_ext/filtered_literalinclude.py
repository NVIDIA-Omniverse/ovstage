# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

"""Sphinx extension: filtered-literalinclude directive.

Extends ``literalinclude`` with named snippet selection and filtering:

``:snippets:``
    Comma-separated snippet names from one source file. Extracts each marked
    region, dedents it independently, and joins the regions with a blank line.
    Cannot be combined with literalinclude's line selection or dedent options.

``:exclude-pattern:``
    Strips lines matching a regular expression.  The primary use-case is
    removing ``# [snippet:...]`` / ``// [snippet:...]`` markers that are
    present in example files for skill references but should not appear in
    rendered documentation.

``:omit-markers:``
    Strips entire regions delimited by ``# [omit]`` / ``# [/omit]`` (Python)
    or ``// [omit]`` / ``// [/omit]`` (C/C++) marker pairs.  Both the marker
    lines and all lines between them are removed.  Use this to hide
    test-only code (asserts, debug helpers) that lives inside a snippet but
    should not appear in rendered docs.
"""

import re
import textwrap

from docutils import nodes as docutils_nodes

from sphinx.directives.code import LiteralInclude


class FilteredLiteralInclude(LiteralInclude):
    """A ``literalinclude`` variant that can exclude lines by regex."""

    option_spec = {
        **LiteralInclude.option_spec,
        "snippets": str,
        "exclude-pattern": str,
        "omit-markers": lambda x: True,  # flag option, no value
    }

    # Matches # [omit] / # [/omit] and // [omit] / // [/omit]
    _OMIT_OPEN = re.compile(r"^\s*(?:#|//)\s*\[omit\]\s*$")
    _OMIT_CLOSE = re.compile(r"^\s*(?:#|//)\s*\[/omit\]\s*$")

    def _apply_filters(self, text: str) -> str:
        lines = text.splitlines()

        # Strip omit regions first (order doesn't matter, but do it first
        # so exclude-pattern doesn't need to match inside omitted blocks).
        if "omit-markers" in self.options:
            kept = []
            omitting = False
            for line in lines:
                if self._OMIT_OPEN.search(line):
                    omitting = True
                    continue
                if self._OMIT_CLOSE.search(line):
                    omitting = False
                    continue
                if not omitting:
                    kept.append(line)
            lines = kept

        # Then strip individual lines matching exclude-pattern.
        pattern = self.options.get("exclude-pattern")
        if pattern:
            regex = re.compile(pattern)
            lines = [line for line in lines if not regex.search(line)]

        return "\n".join(lines)

    def run(self):
        """Select named regions before filtering, preserving Sphinx file tracking."""
        snippet_names = self.options.get("snippets")
        if snippet_names is not None:
            # These options would alter the source before marker extraction.
            incompatible = {"lines", "start-at", "start-after", "end-at", "end-before",
                            "pyobject", "diff", "dedent", "prepend", "append",
                            "lineno-match"}.intersection(self.options)
            if incompatible:
                return [self.state_machine.reporter.warning(
                    ":snippets: cannot be combined with " + ", ".join(sorted(incompatible)),
                    line=self.lineno)]

        result = super().run()
        if not (snippet_names is not None or self.options.get("exclude-pattern")
                or "omit-markers" in self.options):
            return result

        # Captions wrap the literal block; leave warnings and wrapper text alone.
        for root in result:
            for node in root.findall(docutils_nodes.literal_block):
                text = node.astext()
                if snippet_names is not None:
                    parts = []
                    for name in snippet_names.split(","):
                        name = name.strip()
                        marker = re.escape(name)
                        opening = list(re.finditer(
                            rf"(?m)^[ \t]*(?:#|//)[ \t]*\[snippet:{marker}\][ \t]*$", text))
                        closing = list(re.finditer(
                            rf"(?m)^[ \t]*(?:#|//)[ \t]*\[/snippet:{marker}\][ \t]*$", text))
                        if (not name or len(opening) != 1 or len(closing) != 1
                                or opening[0].end() >= closing[0].start()):
                            return [self.state_machine.reporter.warning(
                                f"Invalid or missing snippet marker pair: {name!r}",
                                line=self.lineno)]
                        body = text[opening[0].end():closing[0].start()]
                        body = re.sub(
                            r"(?m)^[ \t]*(?:#|//)[ \t]*\[/?snippet:[A-Za-z0-9_.:-]+\][ \t]*\n?",
                            "", body)
                        parts.append(textwrap.dedent(body).strip("\n"))
                    text = "\n\n".join(parts)
                filtered = self._apply_filters(text)
                node.rawsource = filtered
                node.children[:] = [docutils_nodes.Text(filtered)]
        return result


def setup(app):
    app.add_directive("filtered-literalinclude", FilteredLiteralInclude)
    return {"version": "0.1", "parallel_read_safe": True, "parallel_write_safe": True}
