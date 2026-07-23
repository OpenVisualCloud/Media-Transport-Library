---
description: "Use when authoring or refactoring pytest cases under tests/validation/tests/** or the shared engine under tests/validation/mtl_engine/**. Defines the functionality-first paradigm: tests verify a behavior (not a framework), parametrize by application, push shared/library-level logic into the Application base class, and keep app-specific translation in each adapter via universal params."
name: "MTL Validation — Test Authoring Paradigm"
applyTo: "tests/validation/tests/**,tests/validation/mtl_engine/**"
---

# Authoring MTL validation tests: test the functionality, not the framework

A validation test verifies an MTL **behavior** (a frame rate is honored, late
frames are dropped, latency stays bounded). It must not be written against one
framework's mechanics. Follow this paradigm — [test_fps.py](../../tests/validation/tests/single/st20p/test_fps.py)
is the reference.

## 1. Parametrize by `application`

```python
@pytest.mark.parametrize("application", ["rxtxapp", "ffmpeg"])
def test_st20p_xxx(application, app_factory, ...):
    app = app_factory(application)
    app.create_command(**config_params)   # universal params only
    app.execute_test(build=mtl_path, host=host, ...)
```

List **only** the applications that actually support the behavior under test.
If the FFmpeg plugin has no control for a feature (e.g. drop-when-late), the
list is just `["rxtxapp"]` — add the others when they gain support. Do not fork
the test per framework.

## 2. Drive everything through universal params

Tests pass framework-neutral `config_params`; each adapter translates them.
When a feature needs a new knob:

- add it to `UNIVERSAL_PARAMS` in
  [config/universal_params.py](../../tests/validation/mtl_engine/config/universal_params.py)
  (else `set_params` raises `Unknown parameter`);
- wire the translation in each adapter's config/command builder
  (`rxtxapp.py`, `ffmpeg.py`, `GstreamerApp.py`) — only in the ones that
  support it.

Never hardcode a framework's config shape (JSON keys, CLI flags) in the test.

## 3. Push shared logic into the `Application` base class

Anything not specific to one adapter lives in
[application_base.py](../../tests/validation/mtl_engine/application_base.py),
so every framework reuses it. In particular, **output emitted by libmtl itself
is framework-agnostic** — parse it in the base class, not in a test or a single
adapter.

Example: `Application.count_tx_dropped_frames()` parses the library's
`TX_st2Xp(...) ... drop D` stats line (emitted whether RxTxApp, FFmpeg, or
GStreamer drives MTL) and returns the summed drops. The test just calls
`app.count_tx_dropped_frames()` and asserts — it never greps a log itself.

Rule of thumb: if two adapters would copy the same helper, it belongs in the
base class. If a test greps `self.last_output` (a `str`; split on `"\n"`),
that grep almost certainly belongs in the base class or the adapter.

## 4. Keep the test body about the behavior

Build config → `create_command` → `execute_test` → assert via base-class /
engine helpers. Use `fail_on_error=False` when the behavior deliberately breaks
the default RX/TX result gate (e.g. dropped frames), and assert on the specific
metric instead.
