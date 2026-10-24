#!/usr/bin/env python3
"""The root README.md as Doxygen's main page reads it (the Doxyfile's FILTER_PATTERNS).

The README's links are relative to the root of the repository - docs/index.md, CLAUDE.md - which GitHub renders in
place. The generated pages sit under build/doxygen/html/, where those paths name nothing, and Doxygen takes a link to a
Markdown file it was not given as a reference it cannot resolve. So each relative link is rewritten to the file on the
repository's default branch, which is where a reader of the documentation finds the design documents. A link with a
scheme (https:, mailto:) or to an anchor on the page itself is left alone; nothing else in the file changes, line for
line, so Doxygen's warnings keep the README's own line numbers.

    python3 tools/doxygen_readme_filter.py README.md    # the filtered text on stdout
"""
import re
import sys

REPOSITORY = "https://github.com/reckless-technology/reckless-toolkit/blob/main/"

# A Markdown inline link's target: ](target) or ](target "title"), the target with no space in it.
LINK = re.compile(r"\]\((?P<target>[^)\s]+)(?P<rest>(?:\s+\"[^\"]*\")?)\)")


def rewrite(match: re.Match) -> str:
    """One link, its target made absolute on the repository when it is relative to the repository's root.

    @param match  a match of LINK: the target and an optional title
    @return       the link's text with its target rewritten, or unchanged when it has a scheme or is an anchor
    """
    target = match.group("target")
    if re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target) or target.startswith("#"):
        return match.group(0)
    if target.startswith("./"):
        target = target[2:]
    return f"]({REPOSITORY}{target}{match.group('rest')})"


def main() -> int:
    """Write the filtered README named on the command line to stdout.

    @return  the process's exit code: 0, or 2 when no file is named
    """
    if len(sys.argv) != 2:
        print("usage: doxygen_readme_filter.py <README.md>", file=sys.stderr)
        return 2
    with open(sys.argv[1], encoding="utf-8") as f:
        sys.stdout.write(LINK.sub(rewrite, f.read()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
