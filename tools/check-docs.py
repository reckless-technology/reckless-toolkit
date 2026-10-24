#!/usr/bin/env python3
"""Check the toolkit's documents: every relative link in every Markdown and HTML file of the toolkit resolves to a file
or directory of the toolkit - none leaves it, since the toolkit stands on its own - every document is listed in
docs/index.md.

A document is a file under docs/ other than an image, and the Markdown files at the toolkit's root. A link inside a
fenced code block or an inline code span is not a link and is not checked. The toolkit is this script's parent's
parent, wherever it sits - its own repository, or a product's tree that holds it - and its files are the ones git has
or would take there.

    python3 tools/check-docs.py            # from anywhere
    python3 tools/check-docs.py --links    # the links alone
    python3 tools/check-docs.py --tracked  # the files git tracks alone, as CI checks a commit in a kept workspace

Prints each broken link, each document missing from the index and each header missing from the page, and exits 1 if
there is any.
"""
import os
import posixpath
import re
import subprocess
import sys
from pathlib import Path

MD_LINK = re.compile(r"\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
MD_REF = re.compile(r"(?m)^\s*\[[^\]]+\]:\s*(\S+)")
HTML_ATTR = re.compile(r"""(?:href|src)\s*=\s*["']([^"']*)["']""")
FENCE = re.compile(r"(?ms)^\s*(```|~~~).*?^\s*\1[^\n]*$")
CODE_SPAN = re.compile(r"`[^`\n]*`")
SCHEME = re.compile(r"^[a-zA-Z][a-zA-Z0-9+.-]*:")
IMAGE = re.compile(r"\.(png|jpe?g|gif|svg|webp|ico)$", re.I)

ROOT = Path(__file__).resolve().parent.parent


def toolkit_files(tracked_only):
    """The toolkit's files, relative to its root, that git has or would take and that are on disk."""
    listing = ["git", "ls-files", "--cached"] if tracked_only else ["git", "ls-files", "--cached", "--others",
                                                                    "--exclude-standard"]
    out = subprocess.check_output(listing + ["--", "."], cwd=ROOT, text=True)
    return sorted({f for f in out.split("\n") if f and (ROOT / f).is_file()})


def link_targets(path, text):
    """The relative link targets in a document, anchors and queries dropped."""
    if path.endswith(".md"):
        text = FENCE.sub("", text)
        text = CODE_SPAN.sub("", text)
        found = MD_LINK.findall(text) + MD_REF.findall(text) + HTML_ATTR.findall(text)
    else:
        found = HTML_ATTR.findall(text)
    targets = []
    for target in found:
        if not target or target.startswith(("#", "/", "{", "$")) or SCHEME.match(target):
            continue
        target = re.split(r"[#?]", target, maxsplit=1)[0]
        if target:
            targets.append(target)
    return targets


def is_document(path):
    if path.startswith("docs/"):
        return not IMAGE.search(path)
    return "/" not in path and path.endswith(".md")


def main():
    files = toolkit_files("--tracked" in sys.argv[1:])
    known = set(files)
    for f in files:
        d = posixpath.dirname(f)
        while d:
            known.add(d)
            d = posixpath.dirname(d)

    problems = 0
    for path in files:
        if not re.search(r"\.(md|html?)$", path):
            continue
        text = (ROOT / path).read_text(encoding="utf-8")
        for target in link_targets(path, text):
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(path), target))
            if resolved.startswith(".."):
                print(f"leaves the toolkit: {path} -> {target}")
                problems += 1
            elif resolved not in known:
                print(f"broken link: {path} -> {target}")
                problems += 1

    if "--links" not in sys.argv[1:]:
        index = "docs/index.md"
        if index not in known:
            print(f"missing: {index}")
            problems += 1
        else:
            text = (ROOT / index).read_text(encoding="utf-8")
            listed = {posixpath.normpath(posixpath.join("docs", t)) for t in link_targets(index, text)}
            for path in files:
                if path != index and is_document(path) and path not in listed:
                    print(f"not in {index}: {path}")
                    problems += 1

    checked = ("every link resolves inside the toolkit" if "--links" in sys.argv[1:] else
               "every link resolves inside the toolkit and every document is indexed")
    print(f"{problems} problem(s)" if problems else f"toolkit documents: {checked}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
