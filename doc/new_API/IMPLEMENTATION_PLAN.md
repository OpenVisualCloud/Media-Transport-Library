# MTL Unified Session API - Implementation Plan

This document describes the step-by-step process to implement the new unified session API and validate it by migrating test applications.

---

## Resources in This Directory

All design documents and reference implementations are in `doc/new_API/`:

### API Design
| File | Description |
|------|-------------|
| [mtl_session_api_improved.h](mtl_session_api_improved.h) | **Public API header** - Copy to `include/mtl_session_api.h` |
| [mtl_session_internal.h](mtl_session_internal.h) | **Internal header** - Copy to `lib/src/mt_session.h` |
| [GRACEFUL_SHUTDOWN.md](GRACEFUL_SHUTDOWN.md) | Shutdown design rationale and patterns |
| [List-of-changes.md](List-of-changes.md) | Summary of API changes vs current pipeline API |

### Sample Code
All samples in [samples/](samples/) are self-contained demonstrations:

| Sample | Pattern |
|--------|---------|
| [sample-rx-lib-owned.c](samples/sample-rx-lib-owned.c) | Basic RX with library-owned buffers |
| [sample-tx-lib-owned.c](samples/sample-tx-lib-owned.c) | Basic TX with library-owned buffers |
| [sample-rx-app-owned.c](samples/sample-rx-app-owned.c) | RX with user-provided buffers (zero-copy) |
| [sample-tx-app-owned.c](samples/sample-tx-app-owned.c) | TX with user-provided buffers (zero-copy) |
| [sample-rx-slice-mode.c](samples/sample-rx-slice-mode.c) | RX slice mode (ultra-low latency) |
| [sample-tx-slice-mode.c](samples/sample-tx-slice-mode.c) | TX slice mode (ultra-low latency) |
| [sample-tx-st22-plugin.c](samples/sample-tx-st22-plugin.c) | TX with ST2022-6 codec plugins |
| [sample-signal-shutdown.c](samples/sample-signal-shutdown.c) | Signal handler shutdown pattern |

See [samples/README.md](samples/README.md) for detailed usage patterns.

---

## Goal

Replace the current pipeline API (`st20p_tx/rx_*`) with the new unified API (`mtl_session_*`) in:
1. `/tests/tools/RxTxApp/src/rx_st20p_app.c` - RxTxApp test tool
2. `/tests/integration_tests/st20p_test.cpp` - Integration tests

All existing tests must pass with the new API.

---

## Phase 1: Core Implementation (lib/)

### Task 1.1: Create Public Header
**File**: `include/mtl_session_api.h`

Copy from [mtl_session_api_improved.h](mtl_session_api_improved.h) with adjustments:
- [ ] Add proper include guards and copyright
- [ ] Add `#include "mtl_api.h"` for base types
- [ ] Ensure all types are properly exported

**Estimated effort**: 1 hour

### Task 1.2: Create Internal Header  
**File**: `lib/src/mt_session.h`

Based on [mtl_session_internal.h](mtl_session_internal.h):
- [ ] Define `mtl_session_impl` structure (including `volatile bool stopping` flag)
- [ ] Define `mtl_buffer_impl` structure  
- [ ] Define vtable `mtl_session_vtable_t`
- [ ] Add internal function declarations

**Estimated effort**: 2 hours

### Task 1.3: Implement Core Session Functions
**File**: `lib/src/mt_session.c`

Implement polymorphic dispatch layer (see [GRACEFUL_SHUTDOWN.md](GRACEFUL_SHUTDOWN.md) for shutdown design):
```c
// Core functions that dispatch via vtable
int mtl_session_start(mtl_session_t* session);
int mtl_session_stop(mtl_session_t* session);
int mtl_session_destroy(mtl_session_t* session);
int mtl_session_buffer_get(mtl_session_t* session, mtl_buffer_t** buffer, int timeout_ms);
int mtl_session_buffer_put(mtl_session_t* session, mtl_buffer_t* buffer);
int mtl_session_get_stats(mtl_session_t* session, mtl_session_stats_t* stats);
int mtl_session_event_poll(mtl_session_t* session, mtl_event_t* event, int timeout_ms);
```

Reference implementation patterns: [sample-rx-lib-owned.c](samples/sample-rx-lib-owned.c)

**Estimated effort**: 4 hours

### Task 1.4: Implement Video TX Session
**File**: `lib/src/st2110/mt_session_video_tx.c`

Wrap `st_tx_video_session_impl`:
- [ ] `mtl_video_tx_session_create()` - allocate impl, fill vtable, create low-level session
- [ ] `video_tx_buffer_get()` - wrap `st20_tx_get_next_frame()`, check stopping flag
- [ ] `video_tx_buffer_put()` - wrap `st20_tx_put_frame()`
- [ ] `video_tx_start/stop()` - call `mtl_start/stop()` for scheduler
- [ ] Event queue integration for callbacks

Reference: [sample-tx-lib-owned.c](samples/sample-tx-lib-owned.c), [sample-tx-app-owned.c](samples/sample-tx-app-owned.c)

**API Mapping** (see [List-of-changes.md](List-of-changes.md) for full details):
| Low-Level API | New API |
|---------------|---------|
| `st20_tx_create()` | Inside `mtl_video_tx_session_create()` |
| `st20_tx_free()` | Inside `mtl_session_destroy()` |
| `st20_tx_get_next_frame()` | `mtl_session_buffer_get()` |
| `st20_tx_put_frame()` | `mtl_session_buffer_put()` |
| `st20_tx_frame_size()` | `mtl_session_get_frame_size()` |
| `notify_frame_done` callback | `MTL_EVENT_BUFFER_DONE` event |
| `notify_frame_late` callback | `MTL_EVENT_FRAME_LATE` event |
| `notify_event(VSYNC)` callback | `MTL_EVENT_VSYNC` event |

**Estimated effort**: 8 hours

### Task 1.5: Implement Video RX Session
**File**: `lib/src/st2110/mt_session_video_rx.c`

Wrap `st_rx_video_session_impl`:
- [ ] `mtl_video_rx_session_create()` - allocate impl, fill vtable, create low-level session
- [ ] `video_rx_buffer_get()` - wrap `st20_rx_get_frame()`, check stopping flag
- [ ] `video_rx_buffer_put()` - wrap `st20_rx_put_frame()`
- [ ] Event queue for `notify_frame_ready`, `notify_detected`, etc.
- [ ] Auto-detect format support

Reference: [sample-rx-lib-owned.c](samples/sample-rx-lib-owned.c), [sample-rx-app-owned.c](samples/sample-rx-app-owned.c)

**API Mapping** (see [List-of-changes.md](List-of-changes.md) for full details):
| Low-Level API | New API |
|---------------|---------|
| `st20_rx_create()` | Inside `mtl_video_rx_session_create()` |
| `st20_rx_free()` | Inside `mtl_session_destroy()` |
| `st20_rx_get_frame()` | `mtl_session_buffer_get()` |
| `st20_rx_put_frame()` | `mtl_session_buffer_put()` |
| `st20_rx_get_detected_meta()` | `mtl_session_get_detected_format()` |
| `notify_frame_ready` callback | `MTL_EVENT_BUFFER_READY` event |
| `notify_detected` callback | `MTL_EVENT_FORMAT_DETECTED` event |
| `notify_timing_parser_result` | `MTL_EVENT_TIMING_REPORT` event |

**Estimated effort**: 8 hours

### Task 1.6: Implement Buffer Wrapper
**File**: `lib/src/mt_session_buffer.c`

Wrap `st_frame_trans` as `mtl_buffer_impl`:
- [ ] Create buffer from `st_frame_trans`
- [ ] Expose data pointers, sizes, timestamps
- [ ] Handle external buffer mode
- [ ] Reference counting

See `mtl_buffer_t` definition in [mtl_session_api_improved.h](mtl_session_api_improved.h).

**Estimated effort**: 3 hours

### Task 1.7: Implement Event Queue
**File**: `lib/src/mt_session_event.c`

Ring-buffer based event queue:
- [ ] `mtl_event_queue_create()` - allocate ring buffer
- [ ] `mtl_event_queue_push()` - called from callbacks
- [ ] `mtl_event_queue_pop()` - called from `event_poll()`
- [ ] Timeout support with condition variable

See `mtl_event_t` and event types in [mtl_session_api_improved.h](mtl_session_api_improved.h).
Usage pattern: [sample-rx-app-owned.c](samples/sample-rx-app-owned.c), [sample-rx-slice-mode.c](samples/sample-rx-slice-mode.c)

**Estimated effort**: 4 hours

### Task 1.8: Build System Integration
**Files**: `lib/meson.build`, `include/meson.build`

- [ ] Add new source files to build
- [ ] Add new header to install list
- [ ] Ensure proper dependencies

**Estimated effort**: 1 hour

---

## Phase 2: First Validation - RxTxApp

### Task 2.1: Create Adapter Header
**File**: `tests/tools/RxTxApp/src/session_compat.h`

Create compatibility layer for gradual migration:
```c
// Temporary - maps old st20p calls to new API for testing
#ifdef USE_NEW_SESSION_API
  #define ST20P_RX_HANDLE mtl_session_t*
  #define st20p_rx_get_frame(h) mtl_session_buffer_get_compat(h)
  // ... etc
#else
  #define ST20P_RX_HANDLE st20p_rx_handle
  // ... keep old API
#endif
```

**Estimated effort**: 2 hours

### Task 2.2: Migrate rx_st20p_app.c
**File**: `tests/tools/RxTxApp/src/rx_st20p_app.c`

Current pipeline API usage to replace:

| Current Code | New Code |
|--------------|----------|
| `st20p_rx_create(ctx->st, &ops)` | `mtl_video_rx_session_create(ctx->st, &config, &session)` |
| `st20p_rx_get_frame(handle)` | `mtl_session_buffer_get(session, &buf, timeout)` |
| `st20p_rx_put_frame(handle, frame)` | `mtl_session_buffer_put(session, buf)` |
| `st20p_rx_free(handle)` | `mtl_session_destroy(session)` |
| `st20p_rx_wake_block(handle)` | `mtl_session_stop(session)` |
| `st20p_rx_frame_size(handle)` | `mtl_session_get_frame_size(session)` |
| `st20p_rx_pcapng_dump(...)` | `mtl_session_pcap_dump(...)` |
| `struct st_frame*` | `mtl_buffer_t*` |
| `frame->addr[0]` | `buf->data` or `mtl_buffer_get_plane(buf, 0)` |
| `frame->data_size` | `buf->data_size` |
| `frame->timestamp` | `buf->timestamp` |

Migration steps:
- [ ] Replace `struct st_app_rx_st20p_session` handle type
- [ ] Update `app_rx_st20p_init()` to use new create function
- [ ] Update `app_rx_st20p_frame_thread()` to use new buffer API
- [ ] Update `app_rx_st20p_uinit()` to use new destroy function
- [ ] Update `app_rx_st20p_consume_frame()` for new buffer type
- [ ] Test with basic RX scenario

**Estimated effort**: 4 hours

### Task 2.3: Manual Testing
Run RxTxApp with new API:
```bash
# Terminal 1: TX (still using old API)
./build/app/RxTxApp --config_file config/tx_1v.json

# Terminal 2: RX (using new API)  
./build/app/RxTxApp --config_file config/rx_1v.json
```

- [ ] Verify frames received
- [ ] Verify fps matches expected
- [ ] Verify no memory leaks (run with ASAN)
- [ ] Verify stats reporting works

**Estimated effort**: 2 hours

---

## Phase 3: Full Validation - Integration Tests

### Task 3.1: Create Test Compatibility Layer
**File**: `tests/integration_tests/session_test_common.hpp`

Common test utilities for new API:
```cpp
// Wrapper functions for tests
mtl_session_t* test_create_tx_session(tests_context* ctx, mtl_video_config_t* config);
mtl_session_t* test_create_rx_session(tests_context* ctx, mtl_video_config_t* config);
void test_tx_frame_thread_new(void* args);  // Using new API
void test_rx_frame_thread_new(void* args);  // Using new API
```

**Estimated effort**: 3 hours

### Task 3.2: Migrate st20p_test.cpp - Creation/Destruction Tests
**Lines ~280-320**: `TEST(St20p, tx_create_free_*)`, `TEST(St20p, rx_create_free_*)`

Current:
```cpp
tx_handle[i] = st20p_tx_create(st, &ops_tx);
st20p_tx_free(tx_handle[i]);
```

New:
```cpp
mtl_video_tx_session_create(st, &config, &session);
mtl_session_destroy(session);
```

- [ ] Update `st20p_tx_ops_init()` to create `mtl_video_config_t`
- [ ] Update `st20p_rx_ops_init()` to create `mtl_video_config_t`
- [ ] Update all create/free tests

**Estimated effort**: 4 hours

### Task 3.3: Migrate st20p_test.cpp - Data Path Tests
**Lines ~340-600**: `test_st20p_tx_frame_thread()`, `test_st20p_rx_frame_thread()`

Current:
```cpp
frame = st20p_tx_get_frame((st20p_tx_handle)handle);
// fill frame
st20p_tx_put_frame((st20p_tx_handle)handle, frame);
```

New:
```cpp
mtl_buffer_t* buf;
mtl_session_buffer_get(session, &buf, timeout);
// fill buf->data
mtl_session_buffer_put(session, buf);
```

- [ ] Update TX frame thread
- [ ] Update RX frame thread  
- [ ] Handle SHA verification with new buffer type
- [ ] Handle user meta with new API

**Estimated effort**: 6 hours

### Task 3.4: Migrate st20p_test.cpp - Digest Tests
**Lines ~670-1200**: `st20p_rx_digest_test()`

This is the main comprehensive test. Migration requires:
- [ ] Config translation: `st20p_tx_ops` → `mtl_video_config_t`
- [ ] Config translation: `st20p_rx_ops` → `mtl_video_config_t`
- [ ] Handle mapping for all flags
- [ ] External buffer support
- [ ] User timestamp support
- [ ] VSYNC event handling
- [ ] Block/non-block mode

**Estimated effort**: 8 hours

### Task 3.5: Migrate Remaining Tests
- [ ] `st20p_rx_after_start_test()` 
- [ ] `st20p_tx_after_start_test()`
- [ ] Interlace tests
- [ ] RTCP tests
- [ ] Auto-detect tests

**Estimated effort**: 4 hours

### Task 3.6: Run Full Test Suite
```bash
cd build
ninja tests
./tests/KahawaiTest --gtest_filter=St20p.*
```

Expected results:
- [ ] All `St20p.*` tests pass
- [ ] No memory leaks (ASAN)
- [ ] No thread issues (TSAN)

**Estimated effort**: 4 hours

---

## Phase 4: Cleanup and Documentation

### Task 4.1: Remove Compatibility Layer
Once all tests pass:
- [ ] Remove `session_compat.h`
- [ ] Remove `#ifdef USE_NEW_SESSION_API` guards
- [ ] Clean up old includes

**Estimated effort**: 2 hours

### Task 4.2: Documentation
- [ ] Update `doc/` with new API usage guide
- [ ] Add migration guide for external users
- [ ] Update sample applications in `app/sample/`

**Estimated effort**: 4 hours

### Task 4.3: Code Review and Polish
- [ ] Run `./format-coding.sh`
- [ ] Address review comments
- [ ] Final testing

**Estimated effort**: 4 hours

---

## Summary

| Phase | Tasks | Estimated Hours |
|-------|-------|-----------------|
| Phase 1: Core Implementation | 1.1 - 1.8 | 31 hours |
| Phase 2: RxTxApp Validation | 2.1 - 2.3 | 8 hours |
| Phase 3: Integration Tests | 3.1 - 3.6 | 29 hours |
| Phase 4: Cleanup | 4.1 - 4.3 | 10 hours |
| **Total** | | **78 hours** |

## Recommended Order of Implementation

```
Week 1: Phase 1 (Core)
├── Day 1-2: Tasks 1.1-1.3 (Headers + Core dispatch)
├── Day 3-4: Tasks 1.4-1.5 (TX + RX video)
└── Day 5: Tasks 1.6-1.8 (Buffer, Events, Build)

Week 2: Phase 2 + Phase 3 Start
├── Day 1: Task 2.1-2.2 (RxTxApp migration)
├── Day 2: Task 2.3 + 3.1 (Manual test + test utils)
├── Day 3-4: Tasks 3.2-3.3 (Basic test migration)
└── Day 5: Task 3.4 (Digest test)

Week 3: Phase 3 Complete + Phase 4
├── Day 1-2: Tasks 3.5-3.6 (Remaining tests + full run)
└── Day 3-5: Phase 4 (Cleanup + Docs)
```

## Dependencies

```
              ┌─────────────────┐
              │ 1.1 Public Hdr  │
              └────────┬────────┘
                       │
              ┌────────▼────────┐
              │ 1.2 Internal Hdr│
              └────────┬────────┘
                       │
        ┌──────────────┼──────────────┐
        │              │              │
┌───────▼──────┐ ┌─────▼─────┐ ┌──────▼──────┐
│1.4 Video TX  │ │1.5 Video RX│ │1.7 Events  │
└───────┬──────┘ └─────┬─────┘ └──────┬──────┘
        │              │              │
        └──────────────┼──────────────┘
                       │
              ┌────────▼────────┐
              │  1.3 Core Impl  │
              └────────┬────────┘
                       │
              ┌────────▼────────┐
              │  1.8 Build      │
              └────────┬────────┘
                       │
              ┌────────▼────────┐
              │  2.x RxTxApp    │
              └────────┬────────┘
                       │
              ┌────────▼────────┐
              │  3.x Tests      │
              └────────┬────────┘
                       │
              ┌────────▼────────┐
              │  4.x Cleanup    │
              └─────────────────┘
```

## Validation Criteria

### Phase 1 Complete When:
- `./build.sh` succeeds
- New header is installed to `/usr/local/include/`
- Can create/destroy a session without crash

### Phase 2 Complete When:
- RxTxApp receives video frames with new API
- No ASAN errors

### Phase 3 Complete When:
- All `St20p.*` tests pass
- Test coverage same as before migration
- No regressions in CI

### Phase 4 Complete When:
- Documentation complete
- `./format-coding.sh` passes

---

## Files to Create/Modify

### Source Files for New Headers
Copy these from `doc/new_API/` to their target locations:

| Source (Design Doc) | Target (Implementation) |
|---------------------|-------------------------|
| [mtl_session_api_improved.h](mtl_session_api_improved.h) | `include/mtl_session_api.h` |
| [mtl_session_internal.h](mtl_session_internal.h) | `lib/src/mt_session.h` |

### New Implementation Files
```
lib/src/mt_session.c                 # Core polymorphic dispatch
lib/src/st2110/mt_session_video_tx.c # TX video wrapper
lib/src/st2110/mt_session_video_rx.c # RX video wrapper
lib/src/mt_session_buffer.c          # Buffer wrapper
lib/src/mt_session_event.c           # Event queue
tests/integration_tests/session_test_common.hpp # Test utilities
```

### Modified Files
```
lib/meson.build                      # Add new sources
include/meson.build                  # Add new header
tests/tools/RxTxApp/src/rx_st20p_app.c   # Migrate to new API
tests/tools/RxTxApp/src/rx_st20p_app.h   # Update types
tests/integration_tests/st20p_test.cpp   # Migrate tests
tests/integration_tests/tests.hpp        # Add new test context
```

---

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Callback timing differences | Extensive logging during migration |
| Thread safety issues | Run with TSAN, careful review of locks |
| Performance regression | Benchmark before/after |
| External buffer mode broken | Dedicated test cases |
| Memory leaks | ASAN in CI, explicit free checks |

## Rollback Plan

Keep old API functional during development:
1. New API in parallel (not replacing) initially
2. Feature flag to switch between APIs
3. Only remove old API after full validation

---

## Quick Start for Implementers

### Step 1: Understand the Design
1. Read [mtl_session_api_improved.h](mtl_session_api_improved.h) - the public API
2. Read [mtl_session_internal.h](mtl_session_internal.h) - internal structures
3. Read [GRACEFUL_SHUTDOWN.md](GRACEFUL_SHUTDOWN.md) - shutdown patterns
4. Review [List-of-changes.md](List-of-changes.md) - what's different from current API

### Step 2: Study the Samples
Start with simplest patterns:
1. [sample-tx-lib-owned.c](samples/sample-tx-lib-owned.c) - basic TX flow
2. [sample-rx-lib-owned.c](samples/sample-rx-lib-owned.c) - basic RX flow

Then advanced patterns:
3. [sample-tx-app-owned.c](samples/sample-tx-app-owned.c) - zero-copy TX
4. [sample-rx-app-owned.c](samples/sample-rx-app-owned.c) - zero-copy RX
5. [sample-tx-slice-mode.c](samples/sample-tx-slice-mode.c) - ultra-low latency TX
6. [sample-rx-slice-mode.c](samples/sample-rx-slice-mode.c) - ultra-low latency RX
7. [sample-signal-shutdown.c](samples/sample-signal-shutdown.c) - production shutdown

### Step 3: Implementation Order
Follow Phase 1 tasks in order (1.1 → 1.8), then validate with Phase 2.

### Step 4: Validation
```bash
# Build
./build.sh

# Run manual test (Phase 2.3)
./build/app/RxTxApp --config_file config/rx_1v.json

# Run integration tests (Phase 3.6)  
cd build && ./tests/KahawaiTest --gtest_filter=St20p.*

# Check formatting
./format-coding.sh
```

### Directory Structure After Implementation
```
include/
    mtl_session_api.h              # From mtl_session_api_improved.h
lib/src/
    mt_session.h                   # From mtl_session_internal.h
    mt_session.c                   # New: core dispatch
    mt_session_buffer.c            # New: buffer wrapper
    mt_session_event.c             # New: event queue
    st2110/
        mt_session_video_tx.c      # New: TX wrapper
        mt_session_video_rx.c      # New: RX wrapper
```
