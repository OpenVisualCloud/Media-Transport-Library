# Copyright 2026 Intel Corporation
# SPDX-License-Identifier: BSD-3-Clause

"""Send a link to a repository path that is not a document to GitHub.

The Markdown files link to source files, directories, and excluded
Markdown files with a relative path, because GitHub shows them. Sphinx
cannot show a directory or an excluded file, and it copies a source file
as a download. This transform replaces such a link with a link to the
same path on GitHub.
"""

from __future__ import annotations

import os

from docutils import nodes
from sphinx import addnodes
from sphinx.transforms import SphinxTransform


class RepoLinks(SphinxTransform):
    # Run before the environment collectors copy the download files.
    default_priority = 900

    def apply(self, **kwargs) -> None:
        srcdir = os.path.realpath(self.env.srcdir)
        docdir = os.path.dirname(self.env.doc2path(self.env.docname))
        for node in list(self.document.findall(addnodes.download_reference)) + list(
            self.document.findall(addnodes.pending_xref)
        ):
            if node.get("reftype") != "myst":
                continue
            refdomain = node.get("refdomain")
            if refdomain == "doc":
                # A link to a Markdown file that the build excludes.
                if node["reftarget"] in self.env.found_docs:
                    continue
                target = node["reftarget"]
                anchor = node.get("reftargetid")
                path = next(
                    (
                        os.path.join(srcdir, target + suffix)
                        for suffix in self.config.source_suffix
                        if os.path.isfile(os.path.join(srcdir, target + suffix))
                    ),
                    "",
                )
            elif refdomain:
                continue
            else:
                target, _, anchor = node["reftarget"].partition("#")
                if not target or "://" in target:
                    continue
                path = os.path.realpath(os.path.join(docdir, target))
            if not path.startswith(srcdir + os.sep) or not os.path.exists(path):
                continue
            relpath = os.path.relpath(path, srcdir).replace(os.sep, "/")
            refuri = f"{self.config.repo_links_url}/{relpath}"
            if anchor:
                refuri += f"#{anchor}"
            ref = nodes.reference("", "", internal=False, refuri=refuri)
            ref += node.children or [nodes.literal(target, target)]
            node.replace_self(ref)


def setup(app):
    app.add_config_value(
        "repo_links_url",
        "https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main",
        "env",
    )
    app.add_transform(RepoLinks)
    return {"parallel_read_safe": True, "parallel_write_safe": True}
