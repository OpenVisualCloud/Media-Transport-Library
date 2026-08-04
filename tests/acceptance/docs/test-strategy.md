# Acceptance Test Strategy

This document defines how `tests/acceptance/` selects cases. Framework
architecture and execution are documented in
[`doc/acceptance-design.md`](../../../doc/acceptance-design.md) and
[`doc/acceptance_quickstart.md`](../../../doc/acceptance_quickstart.md).

## Test Behavior, Not Adapters

A test verifies an MTL behavior such as packet pacing, format conversion,
stream integrity, or backend selection. Application adapters are transport
mechanisms for expressing that behavior. They are not a normal Cartesian test
dimension, but their capability coverage must remain visible.

Use RxTxApp for library parameter sweeps. Use FFmpeg and GStreamer in focused
adapter-conformance and interoperability cases. Execute each behavior only
through applications that can express it. Also retain an explicitly skipped
application leg when an adapter cannot express that behavior: the collected
skip is the coverage record and its reason names the missing capability.

Keep capability skips sparse: use the smallest representative input, avoid
duplicating the same gap across unrelated media or parameter axes, and remove
the skip when support lands. Do not use a skip to hide a reachable behavior
that is unfinished or failing; that case needs an executable test or a bug
investigation.

## Parameter Depth

Choose sweep depth from the implementation selected by the parameter.

| Class | Meaning | Coverage |
| --- | --- | --- |
| Path selector | Selects another tasklet, backend, queue model, codec, or hardware engine | Representative load ladder and coupled arithmetic axes |
| Arithmetic modifier | Changes packetization, pacing, timestamp, or buffer calculations | Boundary values and the axis used by the calculation |
| Value parameter | Copied or compared without changing the implementation | Two or three representative values |
| Deployment parameter | Controls placement, logging, or capacity | Performance or dedicated deployment tests |

Do not build full Cartesian products. Add a second axis only when source code
shows that the two values share control flow or arithmetic. For larger coupled
sets, use pairwise cases.

## Pinned Inputs

A test should vary one named behavior and pin unrelated inputs. Media filenames
must not hide changes in resolution, frame rate, format, or interlace state.
Select explicit entries from `mtl_engine/media_files.py` as load anchors:

| Rung | Media | Purpose |
| --- | --- | --- |
| LS | `i576i50` | Interlaced SD and pacing fallback |
| L0 | `i720p59` | Low-cost progressive stream |
| L1 | `i1080p59` | Baseline functional stream |
| L2 | `i2160p59` | High bandwidth |
| L3 | `i2160p119` | High packet cadence |
| L4 | `i4320p29` | Large frames at throughput comparable to L3 |

Use the smallest subset that reaches the mechanism under test. Expensive
rungs require a mechanism-based justification. Tests that require a specific
pixel or transport format, codec, interlace state, audio layout, ancillary
payload, or metadata item use their specialized registry instead; the generic
video ladder does not replace those behavior-specific inputs.

## Oracles

Every test must declare what makes it pass:

- application result validation;
- media integrity;
- packet compliance; or
- a focused assertion for the selected behavior.

Path-selecting tests must assert the path MTL actually resolved. Requesting a
pacing way, RSS mode, or transport backend is insufficient because MTL can
fall back while continuing to transmit.

An unavailable oracle is explicit. Do not silently omit integrity or
compliance to make a failing case green. Lossy codecs may use application and
compliance results when byte-exact integrity is not a valid property.

## Interoperability

Cross-application coverage is one baseline case for each meaningful TX/RX
application pair. Resolution, frame rate, and codec matrices remain in their
own functional suites unless negotiation specifically depends on them.

GStreamer currently owns both ends of one pipeline and cannot be paired with
a foreign peer through the common adapter. Add those pairs when the adapter
contract can execute them.

## Redundancy

ST 2022-7 is tested as one session with two independent member streams. Each
member uses a distinct interface path and each captured destination must be
included in compliance analysis. A redundant result is valid only when both
member streams satisfy their required oracle.

## Assets

Acceptance assets must be deterministic and registered in
`mtl_engine/media_files.py`. Generation metadata must agree with the declared
pixel and transport formats. New tests must not depend on an undocumented
host-local media corpus.

## Review Checklist

Before merging a matrix change, verify that:

1. The test names one behavior and its additional axes have a source-level
   mechanism.
2. Every parameter is part of the universal vocabulary and translated only by
   adapters that support it.
3. Unsupported application combinations have a precise, collected skip at a
   representative input; supported combinations remain executable.
4. Every case has a meaningful oracle, including a resolved-path assertion
   for path selectors.
5. Collection succeeds and the changed matrix size is intentional.
6. Representative hardware-backed cases pass on every required NIC family or
   carry a precise hardware capability marker.
