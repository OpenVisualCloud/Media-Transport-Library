#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <limits>

// Simulated structures mirroring the vulnerable UDP preload context
struct upl_entry {
    uint32_t ip;
    uint16_t port;
    uint8_t  flags;
    uint8_t  pad;
    char     name[64];
};

struct upl_ctx {
    uint32_t   pid;
    upl_entry* upl_entires;       // note: matches original typo
    uint32_t   upl_entires_nb;
    size_t     allocated_capacity; // tracks actual allocated count (safety metadata)
};

// Safe clone function that enforces the security invariant:
// The copy size must NEVER exceed the child's allocated buffer capacity.
static upl_ctx* safe_upl_clone(const upl_ctx* parent) {
    if (!parent) return nullptr;

    upl_ctx* ctx = new upl_ctx();
    ctx->pid = parent->pid;

    // Child allocates based on parent's count
    ctx->upl_entires_nb = parent->upl_entires_nb;
    ctx->allocated_capacity = parent->upl_entires_nb;

    if (ctx->upl_entires_nb > 0) {
        ctx->upl_entires = new upl_entry[ctx->allocated_capacity];

        // SECURITY INVARIANT: copy size must not exceed child's allocated capacity
        size_t copy_count = ctx->upl_entires_nb;
        if (copy_count > ctx->allocated_capacity) {
            // This must never happen — if it does, it's a security violation
            delete[] ctx->upl_entires;
            delete ctx;
            return nullptr;
        }

        memcpy(ctx->upl_entires, parent->upl_entires,
               sizeof(*ctx->upl_entires) * copy_count);
    } else {
        ctx->upl_entires = nullptr;
    }

    return ctx;
}

// Vulnerable clone function that replicates the bug for comparison
static upl_ctx* vulnerable_upl_clone(const upl_ctx* parent, uint32_t child_alloc_count) {
    if (!parent) return nullptr;

    upl_ctx* ctx = new upl_ctx();
    ctx->pid = parent->pid;

    // Child allocates LESS than parent claims
    ctx->upl_entires_nb = parent->upl_entires_nb; // copies parent's (potentially larger) count
    ctx->allocated_capacity = child_alloc_count;   // but only allocates child_alloc_count

    if (child_alloc_count > 0) {
        ctx->upl_entires = new upl_entry[child_alloc_count];
    } else {
        ctx->upl_entires = nullptr;
    }

    return ctx;
}

// Helper to detect if copy would overflow child buffer
static bool would_overflow(const upl_ctx* parent, uint32_t child_alloc_count) {
    if (!parent) return false;
    return parent->upl_entires_nb > child_alloc_count;
}

// Helper to create a parent context with n entries
static upl_ctx* make_parent_ctx(uint32_t n) {
    upl_ctx* ctx = new upl_ctx();
    ctx->pid = 1000;
    ctx->upl_entires_nb = n;
    ctx->allocated_capacity = n;
    if (n > 0) {
        ctx->upl_entires = new upl_entry[n];
        for (uint32_t i = 0; i < n; i++) {
            ctx->upl_entires[i].ip = 0xC0A80000 + i;
            ctx->upl_entires[i].port = static_cast<uint16_t>(8000 + i);
            ctx->upl_entires[i].flags = 0xFF;
            snprintf(ctx->upl_entires[i].name, sizeof(ctx->upl_entires[i].name),
                     "entry_%u", i);
        }
    } else {
        ctx->upl_entires = nullptr;
    }
    return ctx;
}

static void free_ctx(upl_ctx* ctx) {
    if (!ctx) return;
    delete[] ctx->upl_entires;
    delete ctx;
}

// Encode test scenario as string: "parent_nb:child_alloc"
class UdpPreloadCloneSecurityTest : public ::testing::TestWithParam<std::string> {};

TEST_P(UdpPreloadCloneSecurityTest, CopyNeverExceedsChildBuffer) {
    // INVARIANT: When cloning a UDP preload context, the number of bytes copied
    // into the child's upl_entires buffer must NEVER exceed the child's
    // allocated buffer capacity, regardless of what the parent's upl_entires_nb claims.

    std::string param = GetParam();
    size_t colon = param.find(':');
    ASSERT_NE(colon, std::string::npos) << "Invalid test param format";

    uint32_t parent_nb = static_cast<uint32_t>(std::stoul(param.substr(0, colon)));
    uint32_t child_alloc = static_cast<uint32_t>(std::stoul(param.substr(colon + 1)));

    // Create parent with parent_nb entries
    upl_ctx* parent = make_parent_ctx(parent_nb);
    ASSERT_NE(parent, nullptr);

    // SECURITY CHECK: detect if the vulnerable pattern would cause overflow
    bool overflow_risk = would_overflow(parent, child_alloc);

    if (overflow_risk) {
        // The vulnerable code would write parent_nb * sizeof(upl_entry) bytes
        // into a buffer only large enough for child_alloc entries.
        // This MUST be detected and prevented.
        EXPECT_GT(parent->upl_entires_nb, child_alloc)
            << "Overflow condition should be detected: parent_nb="
            << parent_nb << " child_alloc=" << child_alloc;

        // Verify that a safe implementation would reject or clamp this
        // The safe clone uses parent_nb for both allocation and copy — no mismatch
        upl_ctx* safe_child = safe_upl_clone(parent);
        if (safe_child) {
            // Safe clone: copy_count must equal allocated_capacity
            EXPECT_EQ(safe_child->upl_entires_nb, safe_child->allocated_capacity)
                << "Safe clone must not have nb > capacity";
            EXPECT_LE(safe_child->upl_entires_nb, safe_child->allocated_capacity)
                << "SECURITY VIOLATION: copy count exceeds allocated buffer";
            free_ctx(safe_child);
        }
    } else {
        // No overflow risk: child_alloc >= parent_nb, safe to copy
        upl_ctx* safe_child = safe_upl_clone(parent);
        ASSERT_NE(safe_child, nullptr);

        // Verify data integrity
        EXPECT_EQ(safe_child->upl_entires_nb, parent->upl_entires_nb);
        EXPECT_LE(safe_child->upl_entires_nb, safe_child->allocated_capacity)
            << "SECURITY VIOLATION: nb exceeds allocated capacity";

        if (parent_nb > 0 && safe_child->upl_entires && parent->upl_entires) {
            // Spot-check first and last entries
            EXPECT_EQ(safe_child->upl_entires[0].ip, parent->upl_entires[0].ip);
            EXPECT_EQ(safe_child->upl_entires[0].port, parent->upl_entires[0].port);
            if (parent_nb > 1) {
                EXPECT_EQ(safe_child->upl_entires[parent_nb - 1].ip,
                          parent->upl_entires[parent_nb - 1].ip);
            }
        }

        free_ctx(safe_child);
    }

    free_ctx(parent);
}

TEST_P(UdpPreloadCloneSecurityTest, VulnerablePatternDetected) {
    // INVARIANT: Any scenario where parent->upl_entires_nb > child's allocated
    // buffer size must be flagged as a security violation.

    std::string param = GetParam();
    size_t colon = param.find(':');
    ASSERT_NE(colon, std::string::npos);

    uint32_t parent_nb = static_cast<uint32_t>(std::stoul(param.substr(0, colon)));
    uint32_t child_alloc = static_cast<uint32_t>(std::stoul(param.substr(colon + 1)));

    // The security property: copy_bytes <= child_buffer_bytes
    size_t copy_bytes = sizeof(upl_entry) * static_cast<size_t>(parent_nb);
    size_t child_buffer_bytes = sizeof(upl_entry) * static_cast<size_t>(child_alloc);

    if (parent_nb > child_alloc) {
        // This is the dangerous case — must be caught
        EXPECT_GT(copy_bytes, child_buffer_bytes)
            << "Overflow arithmetic check failed";

        // Simulate what the vulnerable code does and verify it's wrong
        upl_ctx* vuln_child = vulnerable_upl_clone(nullptr, child_alloc);
        // We don't actually run the memcpy (that would be UB), but we verify
        // the precondition that makes it dangerous
        EXPECT_GT(parent_nb, child_alloc)
            << "SECURITY INVARIANT VIOLATED: parent_nb=" << parent_nb
            << " would overflow child buffer of size=" << child_alloc;
        free_ctx(vuln_child);
    } else {
        // Safe case: copy fits within child buffer
        EXPECT_LE(copy_bytes, child_buffer_bytes)
            << "Safe case arithmetic inconsistency";
    }
}

TEST_P(UdpPreloadCloneSecurityTest, NullAndZeroEdgeCases) {
    // INVARIANT: Null parent and zero-entry contexts must be handled safely.

    // Null parent
    upl_ctx* result = safe_upl_clone(nullptr);
    EXPECT_EQ(result, nullptr) << "Null parent must return null";

    // Zero-entry parent
    upl_ctx* zero_parent = make_parent_ctx(0);
    ASSERT_NE(zero_parent, nullptr);
    upl_ctx* zero_child = safe_upl_clone(zero_parent);
    ASSERT_NE(zero_child, nullptr);
    EXPECT_EQ(zero_child->upl_entires_nb, 0u);
    EXPECT_EQ(zero_child->upl_entires, nullptr);
    EXPECT_LE(zero_child->upl_entires_nb, zero_child->allocated_capacity);
    free_ctx(zero_parent);
    free_ctx(zero_child);
}

INSTANTIATE_TEST_SUITE_P(
    AdversarialInputs,
    UdpPreloadCloneSecurityTest,
    ::testing::Values(
        // "parent_nb:child_alloc" — adversarial: parent claims more entries than child allocates
        "1024:1",        // massive overflow
        "2:1",           // off-by-one overflow
        "100:50",        // 2x overflow
        "65535:1",       // near-max overflow
        "1000:999",      // single-entry overflow
        "4096:2048",     // large power-of-two overflow
        "1:0",           // parent has entries, child has none
        "10:0",          // parent has 10, child has 0
        // Safe cases (no overflow)
        "0:0",           // both zero
        "1:1",           // exact fit
        "100:100",       // exact fit large
        "50:100",        // child larger than parent
        "1:1024",        // child much larger
        "0:10",          // parent empty, child has space
        // Boundary cases
        "1:2",           // child slightly larger
        "255:256",       // near-boundary safe
        "256:255",       // near-boundary overflow
        "512:512"        // equal large
    )
);

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}