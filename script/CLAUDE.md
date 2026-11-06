# script/ — user scripts

A person runs these scripts by hand. Each one sources `common.sh`, has a
`show_help()` and a `main()`, and ends with the sourced guard. `common.sh`, the
`.env` lists and `hardening_flags.py` are what the scripts and the builds share,
not user scripts. The rules, and the table of which layer configures which host
setting:

@../.github/instructions/mtl-scripts.instructions.md
