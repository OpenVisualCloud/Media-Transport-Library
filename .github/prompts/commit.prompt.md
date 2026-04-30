---
description: "Stage and commit the current working-tree changes as atomic, signed-off commits whose message length scales with the change."
mode: agent
---

# Commit current changes

## Process

1. Run `git status --short` and `git diff --stat`.
2. **One commit = one logical change.** Split unrelated edits with
   `git add -p`. Never bundle drive-by changes into an unrelated fix.
3. For each commit: stage, draft the message, run `git commit -s`
   immediately. Do **not** wait for approval; the user reviews `git log`
   and reverts/amends if needed. Print the staged files and final message
   after each commit.
4. **Always pass `-s`** so a `Signed-off-by:` trailer is added.
5. Do **not** `git push`. Do **not** use `--no-verify`. Do **not** amend
   commits that may already be pushed unless explicitly told to.

## Format

```text
<Type>: <subject>

<body — optional>

<footers — optional>
```

- **Type** (capitalised): `Feat`, `Add`, `Fix`, `Refactor`, `Docs`, `Test`,
  `Perf`, `Build`, `Ci`, `Style`. See [doc/coding_standard.md](../../doc/coding_standard.md).
- This project does **not** use Conventional-Commits scopes (`Fix(parser):`)
  or breaking markers (`!`). Flag a breaking API/behaviour change with a
  `BREAKING-CHANGE:` footer instead.

## Subject line

- ≤ 50 chars ideal, 72 hard.
- Capitalise the first word after the type prefix. No trailing period.
- Imperative mood. The subject must complete the sentence:
  *"If applied, this commit will __________."*
- Name the symbol, file, or subsystem in the subject itself.
  `Fix: Reject zero-length frames in st20_tx_xmit` — never `Fix bug`
  or `Update stuff`.

## Body — when and what

Include a body only when the change is not obvious from the diff.

- Explain **why** (motivation, the problem, the invariant being
  preserved or broken) and the **non-obvious what**. The diff already
  shows *how*.
- Wrap at 72 cols. Aim for ≤ 5 lines; risky / cross-cutting changes
  may justify more, but every extra line earns its place.
- **Don't re-narrate the diff.** If a sentence enumerates items the
  diff already adds verbatim, delete it.
- **Don't restate linked content.** Reference it (`Refs: #123`,
  `See also: doc/foo.md`) instead of summarising.
- For `Fix`: state the symptom + root cause in one or two lines.
- For `Perf`: include before/after numbers when measured.

## Footers (git-trailer format)

Blank line above; one per line; `Token: value` form.

- `Signed-off-by: Name <email>` — added by `-s`.
- `Fixes: <12-char SHA> ("<subject>")` — recommended for bugfixes;
  helps `git bisect`.
- `Refs: #123` / `Closes: #123`.
- `BREAKING-CHANGE: <description>` — if not flagged with `!` in the subject.

## Never appears

- **Chat / process leakage.** No `I`, `we`, `the agent`, `the user`,
  `per the discussion`, `as requested`, `Copilot`, `AI`.
- **Meta-commentary about the message.** Do not describe the message's
  audience, its structure, or the rules used to write it. The reader
  *is* the audience — they don't need to be told.
- **Vague subjects** that would fit any commit (`Fix bug`, `Update X`,
  `Misc cleanup`).

## Length-vs-importance

- **Trivial** (typo, formatting, comment): subject only.
- **Behaviour or API change**: subject + 2-5 line body explaining why.
- **Risky / cross-cutting / perf**: subject + full body. Perf needs
  before/after numbers; `Fix` needs symptom + root cause.

## Pre-commit self-check

Re-read the drafted message and verify:

1. Subject completes *"If applied, this commit will …"*?
2. Every body sentence describes a change to code, files, or behaviour
   — not the message, not the chat that produced it?
3. No sentence re-narrates content the diff already adds verbatim?
4. Footers in trailer format, blank line above, one per line?

If any answer is no, edit before running `git commit -s`.
