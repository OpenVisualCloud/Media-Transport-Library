# tests/acceptance/ — pytest E2E suite

Running, selecting, and triaging these tests:

@../../.github/instructions/mtl-acceptance-tests.instructions.md

Also relevant, read on demand rather than always:

- Authoring a new case — `../../.github/instructions/mtl-acceptance-authoring.instructions.md`
- Harness internals — `../../.github/instructions/mtl-acceptance-harness.instructions.md`
- Engine internals — `../../.github/instructions/mtl-acceptance-engine.instructions.md`

Host prep is `.github/scripts/acceptance_setup.sh` (interactive, or `--auto`), or the
`mcp__mtl-acceptance-setup__*` tools — fetch their schemas with `ToolSearch` first. There is no
dedicated agent for this tree; the main agent owns it.
