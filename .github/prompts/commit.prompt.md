---
description: "Stage and commit the current working-tree changes as atomic, signed-off commits whose message length scales with the change."
mode: agent
---

# Commit current changes

Follow the [`mtl-commit`](../skills/mtl-commit/SKILL.md) skill: run
`git status`/`git diff --stat`, split into one-logical-change commits,
draft messages per its Format/Subject/Body/Footers rules, then
`git commit -s` each one immediately — no push, no `--no-verify`, no
amending already-pushed commits.
