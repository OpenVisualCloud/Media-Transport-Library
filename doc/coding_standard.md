# Coding standard guideline

## 1. One command

```bash
./checkpatch.sh                  # verify every tracked file -- what CI runs
./checkpatch.sh --staged         # verify staged files -- what the git hook runs
./checkpatch.sh --files a.c b.md # verify specific files
./format-coding.sh               # apply every autofix
./format-coding.sh --staged      # apply the autofixes to staged files only
./format-coding.sh --files a.c   # apply the autofixes to specific files
./format-coding.sh --check       # report, then restore -- same as --preview
./checkpatch.sh --install-hooks  # run the checks automatically on commit
```

Exit status is `0` clean, `1` findings, `2` a usage or environment problem.
Verification runs the real fixers, so a failing run leaves their corrections in
your working tree -- `git diff` is the remediation. `--preview` is the mode that
reports without mutating.

Nothing needs to be installed except `pre-commit` itself, which then installs a
pinned copy of every linter into an isolated environment of its own. They do not
need to be on your `PATH`, and the version you happen to have installed cannot
change the result:

```bash
./checkpatch.sh --bootstrap      # tries pipx, then pip --user
```

[§6.1](#61-installing-the-engine) has the per-platform command if that fails.

## 2. One source of truth

```text
                       .pre-commit-config.yaml         <-- rules live here
                       .github/linters/*               <-- and their rule content
                                  |
             +--------------------+--------------------+
             |                    |                    |
      ./checkpatch.sh      git pre-commit hook   .github/workflows/linter.yml
             |                    |                    |
       ./format-coding.sh    (same hooks)       (same hooks, 3 OSes)
```

Every caller above runs the same hook list from the same file. None of them
contains a rule, a version, a file filter or a tool invocation of its own:

| File | Owns | Must never contain |
|---|---|---|
| `.pre-commit-config.yaml` | which tool, which version, which arguments, which files | nothing else does this |
| `.github/linters/` | rule *content* (`.clang-format` excepted, see below) | tool selection or versions |
| `.editorconfig` | encoding, line endings, and shfmt's shell indent | indent widths the formatters already own |
| `.gitattributes` | line-ending normalization on checkout and check-in | anything else |
| `checkpatch.sh` | which files to feed the engine, how to report | any rule |
| `format-coding.sh` | thin write-mode wrapper over `checkpatch.sh` | any rule |
| `.github/workflows/linter.yml` | orchestration: OS matrix, caching, Python setup | any rule or config path |
| this document | prose rationale and the parity table | rules that are not in the config |

**Adding or changing a lint rule** means editing `.pre-commit-config.yaml` (and
the matching file in `.github/linters/`), and updating the parity table
below. Nowhere else. A rule added to the workflow, to a script, or
to a document is drift by construction, because only the config is what the hooks
actually execute.

**Bumping a tool version** is its own commit, and its message quotes the blast
radius from `./format-coding.sh --check`. Never as a side effect of an unrelated
change: that is how a wrong clang-format silently reformats hundreds of files.
`pre-commit autoupdate` proposes the bumps.

### 2.1. Why `.clang-format` sits at the repository root

Every other rule file lives in `.github/linters/`, because that is where
super-linter looks. `.clang-format` cannot: clang-format itself searches upward
from each source file, so the file has to be at the root, and it is a **real
file, not a symlink**. A Windows checkout without symlink support materializes a
symlink as a text file containing a path, at which point clang-format silently
finds no configuration and falls back to plain LLVM style -- a whole-tree
reformat waiting to happen.

## 3. What is checked

| Language | Tool | Fixes | Rule file |
|---|---|---|---|
| any | pre-commit-hooks 6.0.0 -- five structural guards | no | -- |
| C, C++ | clang-format 22.1.8 | yes | `.clang-format` |
| Python | isort 8.0.1 (`--profile black`) | yes | -- |
| Python | black 26.5.1 (`--line-length 88`) | yes | -- |
| Python | flake8 7.3.0 | no | `.github/linters/.flake8` |
| Python | ruff 0.16.3 | partly | `.github/linters/.ruff.toml` |
| Shell | shfmt 3.13.1 | yes | `.editorconfig` (`[*.sh]`) |
| Shell | shellcheck 0.11.0 | no | -- |
| Markdown | markdownlint-cli 0.49.1 | yes | `.github/linters/.markdown-lint.yml` |
| Markdown prose | textlint 15.8.0 + terminology 4.0.1 | yes | `.github/linters/.textlintrc` |
| YAML | yamllint 1.38.0 | no | `.github/linters/.yaml-lint.yml` |
| GitHub Actions | actionlint 1.7.12 | no | `.github/linters/actionlint.yaml` |
| HTML | htmlhint 1.9.2 | no | `.github/linters/.htmlhintrc` |
| staged diff | gitleaks 8.30.0 | no | -- |

Read the gitleaks row narrowly. Its hook is `gitleaks git --pre-commit --staged`,
so it scans the staged diff and *cannot* scan a whole tree -- in `--all-files`
mode nothing is staged and it passes trivially. It catches a secret you are about
to commit; whole-tree and pull-request scanning is CI's, in the parity table
below.

**A rule file must name its rules, and an argument that matters must be written
down.** Both halves of that were learned the hard way here:

* `.github/linters/.ruff.toml` carries an explicit `select`. A config that names
  no rules describes the pinned version rather than a check -- ruff's implicit
  default went from about 40 rules to 413 between 0.4 and 0.16, so bumping the pin
  turned one hook into 592 findings about blind excepts and datetime timezones,
  none of which anybody had chosen to enforce.
* black's `--line-length 88` is black's own default, spelled out anyway, because
  this document said the Python line length was 120 while the formatter had been
  wrapping at 88 all along. The two numbers are not in conflict -- black *wraps* at
  88, ruff only *rejects* beyond 120 -- but only one of them was written down.

flake8 and ruff both run, and the overlap is deliberate rather than accidental.
`.ruff.toml` selects `E`, `W`, `F` -- flake8's own rule set, since flake8 declares
no `select` either -- so the two are as close to interchangeable as they can be.
They are not yet interchangeable in one direction: **ruff 0.16 does not implement
F824** ("dead `global` declaration") at all; `ruff rule F824` answers "unknown
rule". flake8 7.3 found two of them here, both genuinely dead. That single rule is
the whole reason the flake8 hook still exists, which is also why its rule set is
mirrored rather than extended -- when ruff grows F824, retiring flake8 is a delete
and not a re-measurement.

The five `pre-commit-hooks` entries are not style checks. Each one mechanically
guards a promise made elsewhere in this document and otherwise enforced by nothing:
`destroyed-symlinks` guards [§2.1](#21-why-clang-format-sits-at-the-repository-root),
`check-illegal-windows-names` and `mixed-line-ending` guard
[§6](#6-platforms), `check-merge-conflict` is universal, and `detect-private-key`
is the only secret scan that runs in a bare whole-tree `./checkpatch.sh` -- see the
gitleaks note above for why that gap exists.

**A version bump may not smuggle in a rule change**, and three of the bumps in
this set tried to:

* markdownlint 0.49 ships MD059 and MD060, which did not exist when this config
  was vendored. MD060 alone reports 309 findings. Both are off.
* `textlint-rule-terminology` 5.x rewrote prose in 15 files, including 18 lines of
  published `CHANGELOG.md` history, and replaced "blank line" with "empty line" --
  in a document describing git commit format, where "blank line" is git's own
  wording. The *engine* is bumped to textlint 15.8.0; the *word list* stays at
  terminology 4.0.1, because the word list is rule content, not a tool version.
  It lives at a pinned version for the same reason `.github/linters/` exists.
* clang-format 18 and up reformat hand-laid-out data. See below.

`pymtl_wrap.c` (SWIG-generated) and `vmlinux.h` (kernel-generated) are excluded
from clang-format.

`patches/` is a vendored third-party patch series: reformatting a byte of it would
break `git apply`. That started life as a *global* `exclude: ^patches/`, and a
global exclude was the wrong shape twice over. It protected nothing -- every
formatter here is selected by language type, a `*.patch` file is none of those
types, and deleting the exclude changed no hook's result on the whole tree. And it
silently disabled `check-illegal-windows-names`, which reads paths rather than
content and so was the one hook the exclude could actually reach. With the hook
blinded, `patches/dpdk/26.03/0012-net-ice-e830:-...patch` sat in the tree with a
colon in its name; `git checkout` on Windows rejects that outright with `error:
invalid path` and exit 128, so the repository could not be cloned there at all and
the Windows CI job died during checkout, before any linter ran. The file is renamed
and the exclusion now sits on `mixed-line-ending` -- the only hook that reads every
file regardless of type -- and nowhere else.

C/C++ style is whatever `.clang-format` says -- `BasedOnStyle: Google` with
`ColumnLimit: 90` today. Changing it rewrites the entire tree, so it is its own
commit or it is a mistake.

Two things about clang-format 22 are worth knowing before the next bump, because
both were paid for once already. It no longer reads `(type)-1` as a cast, so
`((mtl_iova_t)-1)` is now written `((mtl_iova_t) - 1)`; that is whitespace only and
the tokens are identical, and in the `((align)-1)` case -- a macro *parameter*, not
a type -- the new spacing is simply correct. And from 18 on, a braced initializer
whose elements carry trailing comments is broken to one element per line. The six
`le10_to_be_*` permute tables in `lib/src/st2110/st_avx512_vbmi.c` are laid out one
pixel group per row precisely so the pattern can be read against the packing it
implements, so they sit inside a `/* clang-format off */` region. That is a pin on
the layout, not an exemption from review: reformatting them cost 480 lines and all
of their readability.

Rules the language tools cannot express -- the two-world rule, prefixes, lock
order, error-return conventions -- are in
[`.github/instructions/mtl-c-coding.instructions.md`](../.github/instructions/mtl-c-coding.instructions.md)
and enforced by review, not by a linter.

### 3.1. Deliberate omissions

Not every available check is enabled. These were considered and rejected, so that
"why isn't there a hook for X" has an answer:

* **`end-of-file-fixer`, `trailing-whitespace`** -- would rewrite 152 and 34
  tracked files respectively on first run. Pure churn against a tree nobody has
  complained about. `.editorconfig` deliberately does not declare
  `insert_final_newline` either: a rule no hook reads would just move the same
  152 violations somewhere less visible.
* **`check-shebang-scripts-are-executable`** -- fails 17 pre-existing files. The
  narrower "`*.sh` must be executable" check that CI already had is kept instead
  (see the parity table).
* **cpplint** -- was configured but never enabled in CI. Its rule set overlaps
  clang-format and contradicts it on line length. Enabling it now would be a
  substantive style change, not a unification.
* **`check-json`** -- fails 20-plus `tests/tools/RxTxApp/script/**/*.json`, which
  use trailing commas. json-c, which RxTxApp actually parses them with, accepts
  those; strict JSON does not. Turning the check on means rewriting working
  fixtures, so it stays off and the fixtures stay as they are.
* **`check-case-conflict`** -- fails, and the failure is a real defect rather than
  a style preference: `tests/acceptance/mtl_engine/RxTxApp.py` and
  `tests/acceptance/mtl_engine/rxtxapp.py` are both tracked, so this tree cannot be
  checked out on a case-insensitive filesystem, which contradicts the macOS and
  Windows support claimed in [§6](#6-platforms). Renaming a module that
  `mtl_engine` imports is not a lint change; the hook is left off until that is
  fixed on its own terms.
* **pylint, hadolint** -- their config files were tracked in `.github/linters/`
  while the matching validators were switched off in CI. The configs were
  deleted; the validators stay off. Re-enabling either is a rule change and needs
  its own discussion.

## 4. Parity table: local, hook and CI

The three must agree. Anything CI enforces that `checkpatch.sh` cannot reproduce
is listed here explicitly, and runs in the `residual-linters` job of
`.github/workflows/linter.yml`.

| CI check | Status | Why |
|---|---|---|
| clang-format, isort, black, flake8, ruff, shfmt, shellcheck, markdownlint, textlint, yamllint, actionlint, htmlhint | in `checkpatch.sh` | pinned by `.pre-commit-config.yaml`; each pin measured clean over the whole tree before adoption |
| `VALIDATE_GITLEAKS` | residual | the hook scans the *staged diff*, so it cannot scan a whole tree or a pull request. Both run the same scanner at different scopes. `detect-private-key` covers the whole tree, but only for PEM blocks |
| `VALIDATE_BASH_EXEC` | residual | `*.sh` must keep its executable bit. No upstream hook has these exact semantics; the nearest one is broader and fails 17 files |
| `VALIDATE_ENV` (dotenv-linter) | residual, **known landmine** | fails all 8 tracked `*.env` whole-tree; see the comment at that key in `linter.yml` |
| `VALIDATE_TYPESCRIPT_ES` | residual | 135 `*.ts` files with no lintable project configuration from a clean clone: `.gitignore`'s blanket `*.json` leaves `tests/tools/perf_debug_mcp/package.json` untracked |
| `VALIDATE_RUST_*`, `VALIDATE_RUST_CLIPPY` | residual | rustfmt and clippy need a Rust toolchain per environment. Carried over unchanged rather than narrowed, since narrowing the edition list would be a rule change |
| `VALIDATE_EDITORCONFIG` | **dropped** | it was default-on before, and enforced nothing: no `.editorconfig` existed. The one that exists now declares only `charset`, `end_of_line` and shfmt's shell indent, all of which the hooks or `.gitattributes` already cover |

That job was a deny-list of nine `VALIDATE_*: false` keys and is now an
**allow-list**, so anything not named above is no longer enforced -- the two real
cases are in the table, and a file type census found no tracked `*.js`, `*.css`,
`*.xml`, `*.go`, `*.rb`, `*.java` or `*.sql` for the rest to have applied to.
super-linter aborts if `true` and `false` are mixed, so no `VALIDATE_*: false`
line may be added back.

### 4.1. The build gate depends on these names

`.github/workflows/build.yml` will not build until every check in this workflow has
passed: its `wait-for-linter` job polls the check runs on the commit, and `build`
declares `needs: [wait-for-linter, checksums]`, so a lint failure skips the build
rather than wasting a DPDK compile on it.

The coupling is by *check-run name*, which is the job's `name:` -- `checkpatch
(ubuntu-latest)`, `checkpatch (macos-latest)`, `checkpatch (windows-latest)` and
`Lint checks not yet in checkpatch`. Nothing validates that the two lists match.
Renaming a job here without updating `build.yml` does not fail the gate, it makes
the gate wait for a check that never arrives and then report a timeout, which looks
like infrastructure flake rather than the configuration error it is. That is how it
broke once: the gate still asked for super-linter's `Lint Code Base`, which stopped
existing the moment `checkpatch` replaced it, and every pull request paid ten
minutes for the privilege. Both files carry a comment saying so.

## 5. Git hooks

```bash
./checkpatch.sh --install-hooks
```

Installs the `pre-commit` and `pre-merge-commit` hooks. They check staged files
only, so they cost roughly the size of your change, not the size of the tree.

To bypass in an emergency:

```bash
git commit --no-verify
git merge --no-verify
```

Bypassing is for a work-in-progress commit or a broken hook, not for merging
unformatted code: CI runs the identical hook list and is the merge authority.

One surprise worth knowing before it costs you ten minutes: if
`.pre-commit-config.yaml` itself has *unstaged* modifications, the hook refuses to
run at all and every commit fails with `Your pre-commit configuration is
unstaged`. Stage the config, or use `--no-verify` while you iterate on it.

## 6. Platforms

Any Linux distribution, macOS and Windows (git-bash / MSYS2) are supported, and
CI runs `./checkpatch.sh` on Linux, macOS and Windows so the claim cannot rot. It
had already rotted once before that job existed: a tracked filename containing a
colon made the tree impossible to check out on Windows (see the `patches/` note in
[§3](#3-what-is-checked)). `check-illegal-windows-names` now fails locally on
any path Windows cannot represent, which is a cheaper place to find out.

### 6.1. Installing the engine

You need `git`, Python 3.10 or newer (what `pre-commit` 4.x requires), and network
access on the first run. Nothing else -- no distribution package of clang-format,
shfmt, shellcheck, Node.js or Go.

```bash
./checkpatch.sh --bootstrap           # pipx if present, else pip --user
```

Most current distributions mark their system Python as externally managed
(PEP 668), which makes `pip install --user` fail. That is not a broken repository
-- it means you should take the distribution's own package, or pipx:

| Platform | Command |
|---|---|
| Fedora, RHEL, CentOS Stream | `sudo dnf install pre-commit` |
| Arch, Manjaro | `sudo pacman -S python-pre-commit` |
| Debian, Ubuntu | `sudo apt install pre-commit` |
| openSUSE | `sudo zypper install python3-pre-commit` |
| macOS | `brew install pre-commit` |
| Windows, git-bash / MSYS2 | `py -m pip install pre-commit` |
| anything with pipx | `pipx install pre-commit` |

Then, once per clone:

```bash
./checkpatch.sh --install-hooks
```

The first run builds the pinned tools into `~/.cache/pre-commit` -- about 560 MB
and a few minutes, once per `.pre-commit-config.yaml` change. `PRE_COMMIT_HOME`
moves it, which is what CI does to keep the cache workspace-relative.

Ten hooks are Python wheels; the other four need no system toolchain either.
`gitleaks` is a Go hook and `pre-commit` downloads Go itself when it is not on
`PATH`. `markdownlint`, `textlint` and `htmlhint` are Node hooks, and the config
pins `default_language_version: node` so `pre-commit` fetches that exact Node
rather than using whatever the host has -- markdownlint-cli 0.49 needs Node 20 or
newer merely to parse its own source, so an older system Node was a crash and a
newer one was an unpinned variable.

One thing genuinely *not* pinned is the engine: the config's floor is
`minimum_pre_commit_version: 3.5.0`, CI runs 4.6.2, and a distribution package
may be older than either. If a hook behaves differently for you than in CI, check
`pre-commit --version` first -- CI is authoritative.

### 6.2. What the scripts avoid

* No GNU-only construct -- no `mapfile`, no `nproc`, no `grep -oP`, no
  `readlink -f`, no in-place `sed -i`. macOS ships bash 3.2 and a BSD userland;
  MSYS2 ships neither GNU coreutils defaults nor a POSIX filesystem.
* Paths are handed to `pre-commit` verbatim, which resolves them itself. Rewriting
  them in shell breaks MSYS2, where `pwd` is not a path native Python can resolve.
* No *linter* configuration file may be a symlink, because a Windows checkout
  materializes it as a text file (§2.1). Non-linter configuration symlinks are
  knowingly exempt.
* Line endings are governed by `.gitattributes`, which converts on *check-in*:
  what is already committed is left alone, so one tracked CRLF Markdown file
  remains and is fine. What must not happen is a *mixed* file, which would fail
  shfmt and clang-format alike -- `mixed-line-ending` is the hook that checks it,
  with `--fix=no`, because normalizing endings is `.gitattributes`' job.

## 7. Commit messages

MTL follows [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/)
with two deviations: structural elements are capitalized, and `Add` is accepted
for `feat` plus `build(deps)` for dependency bumps.

* `Build`: changes to the build tooling or external dependencies
* `Ci`: changes to CI configuration and scripts
* `Docs`: documentation only
* `Feat` / `Add`: a new feature
* `Fix`: a bugfix
* `Perf`: a change that improves performance
* `Refactor`: a change that neither fixes a bug nor adds a feature
* `Style`: changes that do not affect the meaning of the code
* `Test`: adding or correcting tests

This is convention, not automation: `checkpatch.sh` checks *files*, never the
commit message or the patch as a whole. Reviewers enforce the format.
