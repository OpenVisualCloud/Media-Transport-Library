#!/bin/bash
# FreeBSD integration test script

set -e

echo "==== FreeBSD Integration Tests ===="
echo "Date: $(date)"
echo "System: $(uname -a)"

# Color codes for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

pass_count=0
fail_count=0
warn_count=0

test_pass() {
	echo -e "${GREEN}[PASS]${NC} $1"
	((pass_count++))
}

test_fail() {
	echo -e "${RED}[FAIL]${NC} $1"
	((fail_count++))
}

test_warn() {
	echo -e "${YELLOW}[WARN]${NC} $1"
	((warn_count++))
}

echo ""
echo "=== Test 1: Library Build Verification ==="
if [ -f "build/lib/libmtl.so" ] || [ -f "build/lib/libmtl.so.1" ]; then
	test_pass "MTL library file exists"
	ls -lh build/lib/libmtl.so* 2>/dev/null
else
	test_fail "MTL library file not found"
fi

echo ""
echo "=== Test 2: Symbol Exports ==="
if nm build/lib/libmtl.so* 2>/dev/null | grep -q "T mtl_init"; then
	test_pass "mtl_init symbol exported"
else
	test_fail "mtl_init symbol not found"
fi

if nm build/lib/libmtl.so* 2>/dev/null | grep -q "T mtl_start"; then
	test_pass "mtl_start symbol exported"
else
	test_fail "mtl_start symbol not found"
fi

if nm build/lib/libmtl.so* 2>/dev/null | grep -q "T mtl_stop"; then
	test_pass "mtl_stop symbol exported"
else
	test_fail "mtl_stop symbol not found"
fi

echo ""
echo "=== Test 3: FreeBSD Platform Integration ==="
if strings build/lib/libmtl.so* 2>/dev/null | grep -qi "freebsd"; then
	test_pass "FreeBSD platform code present"
else
	test_warn "No FreeBSD strings found (may be optimized out)"
fi

echo ""
echo "=== Test 4: NUMA Support ==="
if pkg info numa >/dev/null 2>&1; then
	echo "System NUMA library available"
	if nm build/lib/libmtl.so* 2>/dev/null | grep -q "numa_available"; then
		test_pass "Using system NUMA library"
	else
		test_fail "NUMA library installed but not linked"
	fi
else
	echo "System NUMA library not available"
	if nm build/lib/libmtl.so* 2>/dev/null | grep -q "numa_available"; then
		test_pass "Using NUMA stubs (expected on single-socket)"
	else
		test_fail "NUMA stubs not found"
	fi
fi

echo ""
echo "=== Test 5: AF_XDP Backend Check ==="
if ! nm build/lib/libmtl.so* 2>/dev/null | grep -q "xdp"; then
	test_pass "AF_XDP backend correctly disabled (Linux-only feature)"
else
	test_warn "XDP symbols found (should be disabled on FreeBSD)"
fi

echo ""
echo "=== Test 6: DPDK Integration ==="
if ldd build/lib/libmtl.so* 2>/dev/null | grep -q "librte_"; then
	test_pass "DPDK libraries linked"
	ldd build/lib/libmtl.so* 2>/dev/null | grep "librte_" | head -3
else
	test_fail "DPDK libraries not linked"
fi

echo ""
echo "=== Test 7: RxTxApp Build ==="
if [ -f "build/tests/tools/RxTxApp/RxTxApp" ]; then
	test_pass "RxTxApp executable exists"

	# Try to get help output (may fail without root/hugepages)
	if ./build/tests/tools/RxTxApp/RxTxApp --help >/dev/null 2>&1; then
		test_pass "RxTxApp help command works"
	else
		test_warn "RxTxApp help failed (may need root or hugepages)"
	fi
else
	test_warn "RxTxApp not found (optional component)"
fi

echo ""
echo "=== Test 8: Platform-Specific Code Guards ==="
# Check that sysfs code is properly guarded
if nm build/lib/libmtl.so* 2>/dev/null | grep -q "mt_sysfs_write"; then
	test_pass "sysfs function present (should handle FreeBSD gracefully)"
else
	test_warn "sysfs function not found (may be optimized out)"
fi

echo ""
echo "=== Test 9: Socket Backend ==="
# Check for socket-related functions
if nm build/lib/libmtl.so* 2>/dev/null | grep -q "socket"; then
	test_pass "Socket backend functions present"
else
	test_warn "Socket backend functions not detected"
fi

echo ""
echo "=== Test 10: PThread Support ==="
if nm build/lib/libmtl.so* 2>/dev/null | grep -q "pthread_"; then
	test_pass "pthread functions linked"
else
	test_fail "pthread functions not found"
fi

echo ""
echo "=========================================="
echo "Test Summary:"
echo -e "${GREEN}Passed:${NC} $pass_count"
echo -e "${RED}Failed:${NC} $fail_count"
echo -e "${YELLOW}Warnings:${NC} $warn_count"
echo "=========================================="

if [ $fail_count -gt 0 ]; then
	echo ""
	echo "Some tests failed. Please review the output above."
	exit 1
elif [ $warn_count -gt 5 ]; then
	echo ""
	echo "Many warnings detected. Review recommended."
	exit 0
else
	echo ""
	echo "All critical tests passed!"
	exit 0
fi
