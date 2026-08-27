# tests/unit/ — UnitTest (no NIC, no root)

Build and run: `./build.sh unit` (configures `build_unit/` with `-Denable_unit_tests=true`,
builds, runs). Single case after a build: `./build_unit/tests/unit/UnitTest --gtest_filter='...'`.

Tier-specific traps live in `README.md` here. The short version: ASan cover is **partial** —
`enable_asan` reaches the `libmtl.so` compile arguments only, so symbols `UnitTest` resolves
from that DSO are checked while the production `.c` files `#include`d into harnesses are not;
drain the process-global harness ring in `TearDown()` or you get "passes alone, fails in
suite"; synthetic mbufs only; EAL init is global and one-shot — the first `ut*_init()` fixes
the DPDK config and later calls cannot change it.

New cases must be added to `unit_sources` in `meson.build`. For the tier decision (unit vs
integration vs NoCtx vs pytest) invoke the `mtl-write-test` skill.
