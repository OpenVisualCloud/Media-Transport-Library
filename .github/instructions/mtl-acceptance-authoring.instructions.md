---
description: "Use when authoring or refactoring pytest cases under tests/acceptance/tests/** or the shared engine under tests/acceptance/mtl_engine/**. Defines the functionality-first paradigm: tests verify a behavior (not a framework), parametrize by application, push shared/library-level logic into the Application base class, and keep app-specific translation in each adapter via universal params."
name: "MTL Acceptance — Test Authoring Paradigm"
applyTo: "tests/acceptance/tests/**,tests/acceptance/mtl_engine/**"
---

# Authoring MTL acceptance tests: test the functionality, not the framework

A acceptance test verifies an MTL **behavior** (a frame rate is honored, a
pixel format round-trips intact, latency stays bounded). It must not be written
against one framework's mechanics. Follow this paradigm — [test_fps.py](../../tests/acceptance/tests/single/st20p/test_fps.py)
is the reference.

Architecture: [doc/acceptance-design.md](../../doc/acceptance-design.md) §3, §5.
Running and debugging: [mtl-acceptance-tests.instructions.md](mtl-acceptance-tests.instructions.md).

## 1. Parametrize by `application`

```python
@pytest.mark.parametrize("application", ["rxtxapp", "ffmpeg"])
def test_st20p_xxx(application, app_factory, ...):
    app = app_factory(application)
    app.create_command(**config_params)   # universal params only
    app.execute_test(build=mtl_path, host=host, ...)
```

Run the behavior only through applications that actually support it. When an
adapter lacks the required control, retain a `pytest.param(...,
marks=pytest.mark.skip(reason=...))` capability leg at one representative
input. Its reason must name the missing support so collection records the
coverage gap. Do not repeat that same skip across unrelated media or parameter
axes, and remove it when support lands. Do not fork the test per framework.

## 2. Drive everything through universal params

Tests pass framework-neutral `config_params`; each adapter translates them.
When a feature needs a new knob:

- add it to `UNIVERSAL_PARAMS` in
  [config/universal_params.py](../../tests/acceptance/mtl_engine/config/universal_params.py)
  (else `set_params` raises `Unknown parameter`);
- wire the translation in each adapter's config/command builder
  (`rxtxapp.py`, `ffmpeg.py`, `GstreamerApp.py`) — only in the ones that
  support it.

Never hardcode a framework's config shape (JSON keys, CLI flags) in the test.

## 3. Push shared logic into the `Application` base class

Anything not specific to one adapter lives in
[application_base.py](../../tests/acceptance/mtl_engine/application_base.py),
so every framework reuses it. In particular, **output emitted by libmtl itself
is framework-agnostic** — the library prints the same session statistics no
matter which application drives it, so parse it in the base class, not in a
test or a single adapter. The test then calls a named helper and asserts on the
returned value.

Rule of thumb: if two adapters would copy the same helper, it belongs in the
base class. If a test greps `self.last_output` (a `str`; split on `"\n"`),
that grep almost certainly belongs in the base class or the adapter.

## 4. Keep the test body about the behavior

Build config → `create_command` → `execute_test` → assert via base-class /
engine helpers. Use `fail_on_error=False` when the behavior under test
deliberately breaks the default RX/TX result gate, and assert on the specific
metric instead.
