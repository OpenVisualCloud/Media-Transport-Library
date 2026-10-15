#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Reference lint for the unified API documents (check.sh lint 11).

usage: check_refs.py [--stats] [--base REV] DOCS_DIR
       check_refs.py --bare FILE...

DOCS_DIR is doc/unified-api/. Checks every Markdown file under it (examples.md included:
it is generated, so a hit there is fixed in sketch/examples.md.in or sketch/examples/),
sketch/examples.md.in, and the text of sketch/include/mtl/experimental/*.h,
sketch/examples/*, sketch/numbers.txt and sketch/check.sh:

  - a section reference qualified by a document, "contract.md §3.5", "README §1",
    "[core.md](core.md) §1–§3, §4.1, and §6", "[README §1, §4](../README.md)",
    "design/wait-tests.md §5", names a numbered heading of that document ("### 3.5 ..."
    or "## 3. ..."), both ends of a range included; "file.md Appendix A" names a heading
    "Appendix A";
  - a relative Markdown link names an existing file, and its #anchor the GitHub slug of a
    heading of the target;
  - with --base REV (a git revision of the repository that holds DOCS_DIR), a reference that
    resolves still names the heading it named at REV: the heading of that number, appendix
    letter or anchor in the target at REV ("git show REV:path") has the same title, its
    number aside. So a section renumbered under an old citation fails as "was ... now ...".
    A citing line added or changed since REV ("git diff REV", the working tree included; a
    file git does not track is all new) is not compared: its author chose the number now. A
    target file, number or anchor that did not exist at REV is not compared either.

Document names in prose: a name ending in .md resolves against doc/unified-api/, then the
file's directory, the repository and doc/; "./x" and "../x" against the file; "doc/..." and
".github/..." against the repository. A bare "README" or "README.md" (no directory) is
doc/unified-api/README.md from every file, sketch/ included: every bare README reference of
the sketch's comments and numbers.txt means that README's §1 (the number homes, the marked
tables), and the documents write "sketch/README.md" for the sketch's own README. A name
without .md is checked only where it is unambiguous: a hyphenated document name written
with the hyphen ("ms1-status §3", "implementation-plan §5"), and "contract" ("contract
§9.1"; in these documents the word alone always means contract.md). "timing §", "core §",
"standards §", "the implementation plan §5" and the like are prose, not references.

Not checked: fenced code blocks of the Markdown files, a § with no document name before it
(a clause of a standard, or a section of the same document).
Prints "FAIL: <file>:<line>: <message>" per bad reference; exits 1 if there is any.

--bare FILE... (a review aid for commit 3, not a check.sh lint) checks instead the bare "§n"
references of each named file of the documents next to sketch/: every § item that no
document name or link stands before, fenced blocks included, names a heading of that file.
A § list after a standard's name (ST 2110-21, RFC 8331, RP 2110-25, TR-10-1, IS-04, AES67,
SMPTE, clause, ...) is the standard's clause and is skipped, and so is a number that is no
heading of the file in a paragraph (or a list's lead-in) that names a standard; the summary
line counts both.
"""

import os
import re
import subprocess
import sys
import unicodedata
from urllib.parse import unquote

NUM = r"\d+(?:\.\d+)*"
# One section item: §n.m, optionally a range to §n.m or n.m (with – or -).
ITEM = r"§(" + NUM + r")(?:\s*[–-]\s*§?(" + NUM + r"))?"
SEP = r"(?:\s*,\s*(?:and\s+|or\s+)?|\s+and\s+|\s+or\s+)"
# An item may carry a remark in parentheses with no § and no document name in it:
# "contract.md §1 (R4, R6), §4" cites contract.md §4 too. A ";" ends the list.
NOTE = r"(?:\s*\((?:(?!\.md|README)[^()§\n])*\))?"
SECLIST = re.compile(ITEM + NOTE + r"(?:" + SEP + ITEM + NOTE + r")*")
ITEM_RE = re.compile(ITEM)
APPENDIX = re.compile(r"Appendix ([A-Z])\b")
# A document name in prose: a path ending in .md, or README.
DOCNAME = re.compile(
    r"(?<![\w/.\-])((?:\.{1,2}/)*(?:[\w.\-]+/)*[\w\-]+\.md|README)(?![\w\-]|\.\w)"
)
LINK = re.compile(
    r"!?\[((?:[^\[\]\n]|\n(?!\s*\n)|\[[^\]\n]*\])*)\]\(\s*<?([^)\s>]*)>?(?:\s+\"[^\"]*\")?\s*\)"
)
FENCE = re.compile(r"^ {0,3}(`{3,}|~{3,})")
HEADING = re.compile(r"^ {0,3}(#{1,6})[ \t]+(.*?)(?:[ \t]+#+)?[ \t]*$")
# --bare: a standard's name ("-40" is ST 2110-40 after a mention of it). Right before a §
# list, with only clauses, tables and annexes between, the list is that standard's; anywhere
# in the paragraph, a number that is no heading of the file is its clause.
STANDARD_NAME = (
    r"\b(?:ST|RP|OV|EG)\s*\d{3,4}(?:-\d+)*|\bRFC\s*\d+|\bTR-\d+(?:-\d+)*|\bIS-\d+"
    r"|\bBCP-\d+(?:-\d+)*|\bAES\d+(?:-\d+)?|\bSMPTE\b|\bIEEE\s?\d+(?:\.\d+)*"
    r"|\bISO(?:/IEC)?\s?\d+(?:-\d+)*|\bBT\.\d+|\bITU-R\b|\bIEC\s?\d+(?:-\d+)*"
    r"|(?<![\w-])-\d{2}\b(?=\s*§)"
)
STANDARD = re.compile(
    r"(?:" + STANDARD_NAME + r"|\b[Cc]lauses?)"
    r"(?::\d{4}|\s?\(\d{4}\))?(?:'s|’s)?"
    r"(?:[\s,;]|§" + NUM + r"|Table \d+|Annex [A-Z]|and\b|or\b|[–-])*$"
)
STANDARD_IN = re.compile(STANDARD_NAME)
BLANK = re.compile(r"\n[ \t]*\n")
BLOCK = re.compile(r"\s*(?:[-*+]\s|\d+\.\s|\||```|~~~)")  # a list, table or fence


def paragraph_of(text, pos):
    """The paragraph of text around pos; a list, table or fenced block also takes the blocks
    before it up to its lead-in (at most three)."""
    lo = max(0, pos - 6000)
    window = text[lo : pos + 3000]
    blanks = list(BLANK.finditer(window))
    starts = [0] + [m.end() for m in blanks]
    ends = [m.start() for m in blanks] + [len(window)]
    paras = list(zip(starts, ends))
    k = next(i for i, (a, b) in enumerate(paras) if pos - lo <= b)
    j = k
    while j > 0 and k - j < 3 and BLOCK.match(window, paras[j][0]):
        j -= 1
    return window[paras[j][0] : paras[k][1]]


class Doc:
    """Headings of one Markdown text: section numbers, appendix letters and slugs, each
    mapped to the titles of its headings (number dropped), which --base compares."""

    def __init__(self, text):
        self.numbers = {}
        self.appendices = {}
        self.slugs = {}
        seen = {}  # GitHub's de-duplication: a repeated slug gets -1, -2, ...
        for line in blank_fences(text).split("\n"):
            m = HEADING.match(line)
            if not m:
                continue
            text = m.group(2)
            title = " ".join(
                re.sub(r"^§?" + NUM + r"\.?(?:\s+|$)", "", text.strip()).split()
            )
            n = re.match(r"§?(" + NUM + r")\.?(?:\s|$)", text)
            if n:
                self.numbers.setdefault(n.group(1), set()).add(title)
            a = re.match(r"Appendix ([A-Z])(?:[.:\s]|$)", text)
            if a:
                self.appendices.setdefault(a.group(1), set()).add(title)
            base = slugify(text)
            slug = base
            while slug in seen:
                seen[base] += 1
                slug = "%s-%d" % (base, seen[base])
            seen[slug] = 0
            self.slugs.setdefault(slug, set()).add(title)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def slugify(text):
    """GitHub's heading anchor: rendered text, lowercase, punctuation dropped, spaces to -."""
    parts = re.split(r"(`+)(.*?)\1", text)  # code spans keep their text as it is
    text = ""
    for i in range(0, len(parts), 3):
        # Outside code: links and images give their text, inline HTML and the
        # _emphasis_ marks go (snake_case stays).
        t = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", parts[i])
        t = re.sub(r"<[^>]+>", "", t)
        t = re.sub(r"(?<!\w)_+|_+(?!\w)", "", t)
        text += t + (parts[i + 2] if i + 2 < len(parts) else "")
    out = []
    for ch in text.lower():
        cat = unicodedata.category(ch)
        if ch == " ":
            out.append("-")
        elif ch == "-" or cat[0] in "LMN" or cat == "Pc":
            out.append(ch)
    return "".join(out)


def blank_fences(text):
    """The text with every fenced code block emptied, line count kept."""
    out = []
    fence = None
    for line in text.split("\n"):
        m = FENCE.match(line)
        if fence is None:
            if m:
                fence = m.group(1)
                out.append("")
            else:
                out.append(line)
        else:
            if m and m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence):
                if not line.strip()[len(m.group(1)) :].strip():
                    fence = None
            out.append("")
    return "\n".join(out)


def git(cwd, *args):
    """The output of a git command, or None when it fails."""
    try:
        r = subprocess.run(
            ["git", "-C", cwd] + list(args),
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        return None
    return r.stdout.decode("utf-8", "replace") if r.returncode == 0 else None


class Base:
    """The repository at REV, for --base: the headings of a target there, and the lines of a
    citing file added or changed since (git diff REV, the working tree included)."""

    def __init__(self, docs, rev):
        top = (git(docs, "rev-parse", "--show-toplevel") or "").strip()
        if (
            not top
            or git(top, "rev-parse", "--verify", "-q", rev + "^{commit}") is None
        ):
            raise ValueError(
                "--base %s: no such revision of a repository with %s" % (rev, docs)
            )
        self.top = os.path.realpath(top)
        self.rev = rev
        self.cache = {}
        here = os.path.relpath(docs, self.top)
        files = git(self.top, "ls-files", "-z", "--", here) or ""
        self.tracked = set(files.split("\0"))
        # Path from the top -> the line numbers added or changed since rev.
        self.changed = {}
        diff = [
            "-c",
            "core.quotePath=false",
            "diff",
            "-U0",
            "--no-color",
            "--no-ext-diff",
        ]
        out = git(self.top, *diff, "--find-renames", rev, "--", here) or ""
        path, header = None, False
        for line in out.split("\n"):
            if line.startswith("diff --git "):
                path, header = None, True
            elif header and line.startswith("+++ "):
                name = line[4:].rstrip("\t")
                path = name[2:] if name.startswith("b/") else None
            elif line.startswith("@@"):
                header = False
                m = re.match(r"@@ -\S+ \+(\d+)(?:,(\d+))? @@", line)
                if path and m:
                    first, n = int(m.group(1)), int(m.group(2) or "1")
                    self.changed.setdefault(path, set()).update(range(first, first + n))

    def path(self, p):
        return os.path.relpath(os.path.realpath(p), self.top)

    def edited(self, src, lines):
        p = self.path(src)
        return p not in self.tracked or any(n in self.changed.get(p, ()) for n in lines)

    def doc(self, target):
        """The headings of target at rev, or None when it did not exist there."""
        p = self.path(target)
        if p not in self.cache:
            text = None
            if not p.startswith(".."):
                text = git(self.top, "show", self.rev + ":" + p)
            self.cache[p] = None if text is None else Doc(text)
        return self.cache[p]


class Lint:
    def __init__(self, docs, base=None):
        self.docs = os.path.realpath(docs)
        self.repo = os.path.dirname(os.path.dirname(self.docs))
        self.base = Base(self.docs, base) if base else None
        self.cache = {}
        self.fails = []  # (relpath, line, message)
        self.stats = {}
        # --bare: the spans of the § lists that a document name or a link qualifies.
        self.spans = None
        # A document of doc/unified-api/ named without .md, where the name cannot be an
        # English word (see the module comment): "contract §9.1", "ms1-status §3".
        stems = sorted(
            (
                f[:-3]
                for f in os.listdir(self.docs)
                if f.endswith(".md") and ("-" in f or f == "contract.md")
            ),
            key=len,
            reverse=True,
        )
        self.stem = re.compile(
            r"(?<![\w/.\-])(" + "|".join(map(re.escape, stems)) + r")(?![\w\-.])"
        )

    def count(self, key, n=1):
        self.stats[key] = self.stats.get(key, 0) + n

    def doc(self, path):
        if path not in self.cache:
            self.cache[path] = Doc(read(path))
        return self.cache[path]

    def rel(self, path):
        r = os.path.relpath(path, self.docs)
        return r if not r.startswith("..") else os.path.relpath(path, self.repo)

    def fail(self, src, text, pos, msg):
        line = text.count("\n", 0, pos) + 1
        self.fails.append((self.rel(src), line, msg))

    def drift(self, src, text, first, at, target, kind, key, label):
        """--base: fail when the heading that key (a section number, appendix letter or
        slug) names in target has another title than at REV, unless a line of the reference
        (where it starts, first, or its item, at) was added or changed since REV."""
        if not self.base:
            return
        old = self.base.doc(target)
        was = getattr(old, kind).get(key) if old else None
        now = getattr(self.doc(target), kind).get(key)
        if not was or not now or was & now:
            return
        lines = {text.count("\n", 0, p) + 1 for p in (first, at)}
        if self.base.edited(src, lines):
            return
        self.count("base: heading changed")
        self.fail(
            src,
            text,
            at,
            '%s: was "%s" at %s, now "%s" (line not edited)'
            % (label, " | ".join(sorted(was)), self.base.rev, " | ".join(sorted(now))),
        )

    def resolve_name(self, src, name):
        """The file a document name in prose means, or None (rules: the module comment)."""
        if name in ("README", "README.md"):
            p = os.path.join(self.docs, "README.md")
            return p if os.path.isfile(p) else None
        here = os.path.dirname(src)
        if name.startswith("doc/unified-api/"):
            cands = [os.path.join(self.repo, name)]
        elif name.startswith(("doc/", ".github/")):
            cands = [os.path.join(self.repo, name)]
        elif name.startswith(("./", "../")):
            cands = [os.path.join(here, name)]
        else:
            cands = [
                os.path.join(d, name)
                for d in (self.docs, here, self.repo, os.path.join(self.repo, "doc"))
            ]
        for c in cands:
            if os.path.isfile(c):
                return os.path.normpath(c)
        return None

    def check_sections(self, src, text, first, pos, target, label, seclist, form):
        """Check the § items of seclist, which starts at text[pos], against target; the
        reference starts at text[first]."""
        doc = self.doc(target)
        for i, m in enumerate(ITEM_RE.finditer(seclist)):
            self.count("form: " + form)
            if i:
                self.count("continuation items")
            start, end = m.group(1), m.group(2)
            if end:
                self.count("ranges")
            at = pos + m.start()
            if start not in doc.numbers:
                self.fail(
                    src,
                    text,
                    at,
                    "%s §%s: no heading %s in %s"
                    % (label, start, start, self.rel(target)),
                )
            else:
                self.drift(
                    src,
                    text,
                    first,
                    at,
                    target,
                    "numbers",
                    start,
                    "%s §%s" % (label, start),
                )
            if end:
                # "§6.1–5" means §6.1–§6.5: the end takes the start's leading numbers.
                sp, ep = start.split("."), end.split(".")
                alt = ".".join(sp[: len(sp) - len(ep)] + ep)
                key = end if end in doc.numbers else alt
                if key in doc.numbers:
                    self.drift(
                        src,
                        text,
                        first,
                        at,
                        target,
                        "numbers",
                        key,
                        "%s §%s" % (label, key),
                    )
                else:
                    self.fail(
                        src,
                        text,
                        at,
                        "%s §%s–§%s: range end §%s has no heading in %s"
                        % (label, start, end, end, self.rel(target)),
                    )

    def check_appendix(self, src, text, first, pos, target, label, letter):
        self.count("form: appendix")
        if letter not in self.doc(target).appendices:
            self.fail(
                src,
                text,
                pos,
                "%s Appendix %s: no heading Appendix %s in %s"
                % (label, letter, letter, self.rel(target)),
            )
        else:
            self.drift(
                src,
                text,
                first,
                pos,
                target,
                "appendices",
                letter,
                "%s Appendix %s" % (label, letter),
            )

    def after(self, src, text, pos, target, label, form):
        """Section items or an appendix right after a document reference ending at pos."""
        m = re.compile(r"[ \t]*\n?[ \t]*").match(text, pos)
        p = m.end()
        s = SECLIST.match(text, p)
        if s:
            self.check_sections(src, text, pos, p, target, label, s.group(0), form)
            return True
        a = APPENDIX.match(text, p)
        if a:
            self.check_appendix(src, text, pos, p, target, label, a.group(1))
            return True
        return False

    def links(self, src, text, base):
        """Check the links of a Markdown text; return it with the links blanked."""
        chars = list(text)
        for m in LINK.finditer(text):
            label, url = m.group(1), m.group(2)
            for i in range(m.start(), m.end()):
                if chars[i] != "\n":
                    chars[i] = " "
            self.mark(text, m.end())
            if re.match(r"[a-zA-Z][a-zA-Z0-9+.-]*:", url) or url.startswith("//"):
                continue
            path, _, anchor = url.partition("#")
            path = unquote(path)
            if path:
                target = os.path.normpath(
                    os.path.join(
                        self.repo if path.startswith("/") else base, path.lstrip("/")
                    )
                )
                self.count("link: file#anchor" if anchor else "link: file")
                if not os.path.exists(target):
                    self.fail(
                        src,
                        text,
                        m.start(),
                        "link %s: no file %s" % (url, self.rel(target)),
                    )
                    continue
            else:
                target = src
            md = os.path.isfile(target) and target.endswith((".md", ".md.in"))
            if anchor:
                if not path:
                    self.count("link: #anchor")
                if md and unquote(anchor) not in self.doc(target).slugs:
                    self.fail(
                        src,
                        text,
                        m.start(),
                        "link %s: no heading with anchor #%s in %s"
                        % (url, anchor, self.rel(target)),
                    )
                elif md:
                    self.drift(
                        src,
                        text,
                        m.start(),
                        m.start(),
                        target,
                        "slugs",
                        unquote(anchor),
                        "link %s" % url,
                    )
            if not md:
                continue
            name = os.path.basename(target)
            # A § in the link text belongs to the target when nothing else stands before it
            # but the target's name: "[README §1](../README.md)", "[§21](#21-...)",
            # "[implementation plan §5.2](../implementation-plan.md#52-tasks)".
            s = SECLIST.search(label)
            if s:
                before = re.sub(r"[`*_\s]+", " ", label[: s.start()]).strip().lower()
                stem = re.sub(r"\.md(\.in)?$", "", name).lower()
                ok = (
                    "",
                    name.lower(),
                    stem,
                    stem.replace("-", " "),
                    self.rel(target).lower(),
                )
                if before in ok or before.endswith("/" + name.lower()):
                    self.check_sections(
                        src,
                        text,
                        m.start(),
                        m.start(1) + s.start(),
                        target,
                        "[%s](%s)" % (label.replace("\n", " "), url),
                        s.group(0),
                        "link text",
                    )
            self.after(
                src,
                text,
                m.end(),
                target,
                "[%s](%s)" % (label.replace("\n", " "), url),
                "link then §",
            )
        return "".join(chars)

    def prose(self, src, text):
        names = [
            (m, "README" if m.group(1) == "README" else "name.md")
            for m in DOCNAME.finditer(text)
        ]
        names += [(m, "name without .md") for m in self.stem.finditer(text)]
        for m, kind in names:
            name = m.group(1)
            p = re.compile(r"[ \t]*\n?[ \t]*").match(text, m.end()).end()
            if not (SECLIST.match(text, p) or APPENDIX.match(text, p)):
                continue
            self.mark(text, m.end())
            if kind == "name without .md":
                target = os.path.join(self.docs, name.replace(" ", "-") + ".md")
            else:
                target = self.resolve_name(src, name)
            if target is None:
                self.count("form: prose, " + kind)
                self.fail(src, text, m.start(), "%s: no such file" % name)
                continue
            self.after(src, text, m.end(), target, name, "prose, " + kind)

    def mark(self, text, pos):
        """--bare: record the § list or appendix right after a name or link ending at pos."""
        if self.spans is None:
            return
        p = re.compile(r"[ \t]*\n?[ \t]*").match(text, pos).end()
        s = SECLIST.match(text, p) or APPENDIX.match(text, p)
        if s:
            self.spans.append(s.span())

    def bare(self, path):
        """--bare: check the bare § items of path against its headings; return their count."""
        base = self.docs if path.endswith("examples.md.in") else os.path.dirname(path)
        kept = (self.fails, self.stats)  # the lint's own results stay out of the report
        self.fails, self.stats, self.spans = [], {}, []
        raw = read(path)
        text = self.links(path, raw.replace("`", " "), base)
        self.prose(path, text)
        chars = list(text)
        for a, b in self.spans:
            for i in range(a, b):
                if chars[i] != "\n":
                    chars[i] = " "
        (self.fails, self.stats), self.spans = kept, None
        text = "".join(chars)
        doc = self.doc(path)
        n = 0
        for s in SECLIST.finditer(text):
            if STANDARD.search(text[max(0, s.start() - 80) : s.start()]):
                self.count("bare: after a standard's name")
                continue
            near = STANDARD_IN.search(paragraph_of(raw, s.start()))
            for m in ITEM_RE.finditer(s.group(0)):
                start, end = m.group(1), m.group(2)
                keys = [start]
                if end:
                    sp, ep = start.split("."), end.split(".")
                    alt = ".".join(sp[: len(sp) - len(ep)] + ep)
                    keys.append(end if end in doc.numbers else alt)
                for key in keys:
                    n += 1
                    if key not in doc.numbers and near:
                        self.count("bare: a clause of the paragraph's standard")
                    elif key not in doc.numbers:
                        self.fail(
                            path,
                            text,
                            s.start() + m.start(),
                            "bare §%s: no heading %s in %s"
                            % (key, key, self.rel(path)),
                        )
        return n

    def markdown(self, path, base):
        text = blank_fences(read(path)).replace("`", " ")
        self.prose(path, self.links(path, text, base))

    def source(self, path):
        # Comment leaders at line starts become spaces, so a reference may wrap.
        text = re.sub(
            r"(?m)^([ \t]*)(\*|//|#)",
            lambda m: m.group(1) + " " * len(m.group(2)),
            read(path),
        )
        self.prose(path, text.replace("`", " "))

    def run(self):
        for root, dirs, files in os.walk(self.docs):
            dirs.sort()
            for f in sorted(files):
                p = os.path.join(root, f)
                if f.endswith(".md"):
                    self.markdown(p, root)
                elif f == "examples.md.in":
                    # Rendered as examples.md, so its links are relative to the documents.
                    self.markdown(p, self.docs)
        sk = os.path.join(self.docs, "sketch")
        srcs = [os.path.join(sk, "numbers.txt"), os.path.join(sk, "check.sh")]
        for d in ("include/mtl/experimental", "examples"):
            dd = os.path.join(sk, d)
            srcs += [
                os.path.join(dd, f)
                for f in sorted(os.listdir(dd))
                if os.path.isfile(os.path.join(dd, f))
            ]
        for p in srcs:
            self.source(p)


def main(argv):
    stats, base, bare, args = False, None, False, []
    it = iter(argv[1:])
    for a in it:
        if a == "--stats":
            stats = True
        elif a == "--bare":
            bare = True
        elif a == "--base":
            base = next(it, "")
        elif a.startswith("--base="):
            base = a[len("--base=") :]
        else:
            args.append(a)
    usage = "usage: check_refs.py [--stats] [--base REV] DOCS_DIR\n       check_refs.py --bare FILE...\n"
    if bare:
        if base is not None or stats or not args or not all(map(os.path.isfile, args)):
            sys.stderr.write(usage)
            return 2
        lint = Lint(os.path.dirname(os.path.dirname(os.path.realpath(__file__))))
        n = sum(lint.bare(os.path.realpath(f)) for f in args)
        for f, line, msg in sorted(set(lint.fails), key=lambda x: (x[0], x[1], x[2])):
            print("FAIL: %s:%d: %s" % (f, line, msg))
        print(
            "bare references: %d checked, %d bad; skipped as clauses: %d lists after a "
            "standard's name, %d numbers in a paragraph that names one"
            % (
                n,
                len(set(lint.fails)),
                lint.stats.get("bare: after a standard's name", 0),
                lint.stats.get("bare: a clause of the paragraph's standard", 0),
            )
        )
        return 1 if lint.fails else 0
    if base == "" or len(args) != 1 or not os.path.isdir(args[0]):
        sys.stderr.write(usage)
        return 2
    try:
        lint = Lint(args[0], base)
    except ValueError as e:
        sys.stderr.write("check_refs.py: %s\n" % e)
        return 2
    lint.run()
    for f, line, msg in sorted(set(lint.fails), key=lambda x: (x[0], x[1], x[2])):
        print("FAIL: %s:%d: %s" % (f, line, msg))
    n = sum(v for k, v in lint.stats.items() if k.startswith(("form: ", "link: ")))
    if stats:
        for k in sorted(lint.stats):
            print("%s: %d" % (k, lint.stats[k]))
    print("references: %d checked, %d bad" % (n, len(set(lint.fails))))
    return 1 if lint.fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
