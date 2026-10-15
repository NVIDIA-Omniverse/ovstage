#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

"""Test rendered snippet extraction; run with the Python package's docs extra."""

from io import StringIO
from pathlib import Path
import tempfile
import unittest

from docutils import nodes
from sphinx.application import Sphinx


class FilteredLiteralIncludeTests(unittest.TestCase):
    def test_nested_markers_are_removed_before_dedent(self):
        """Selecting outer regions preserves code, indentation, and sibling order."""
        extension = Path(__file__).resolve().parents[1] / 'docs/_ext'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'source'
            source.mkdir()
            (source / 'conf.py').write_text(
                f'import sys\nsys.path.insert(0, {str(extension)!r})\n'
                'extensions = ["filtered_literalinclude"]\n', encoding='utf-8')
            page = ['Snippets', '========', '']
            for prefix, suffix in [('#', 'py'), ('//', 'cpp')]:
                # Construct markers so the publication scanner ignores this fixture.
                (source / f'example.{suffix}').write_text(f'''outside
{prefix} [region:outer]
    before
{prefix} [region:inner]
        inside
{prefix} [/region:inner]
    after
{prefix} [/region:outer]
{prefix} [region:sibling]
    separate
{prefix} [/region:sibling]
outside
'''.replace('region:', 'snippet:'), encoding='utf-8')
                page.extend([
                    f'.. filtered-literalinclude:: example.{suffix}',
                    '   :snippets: outer, sibling',
                    f'   :language: {"python" if suffix == "py" else "cpp"}',
                    f'   :caption: {suffix} example', '',
                ])
            (source / 'index.rst').write_text('\n'.join(page), encoding='utf-8')
            warnings = StringIO()
            app = Sphinx(str(source), str(source), str(root / 'html'),
                         str(root / 'doctrees'), 'html', status=StringIO(),
                         warning=warnings, warningiserror=True, freshenv=True)
            app.build(force_all=True)
            self.assertEqual(warnings.getvalue(), '')
            self.assertEqual(app.statuscode, 0)
            blocks = list(app.env.get_doctree('index').findall(nodes.literal_block))
            self.assertEqual([block.astext() for block in blocks],
                             ['before\n    inside\nafter\n\nseparate'] * 2)


if __name__ == '__main__':
    unittest.main()
