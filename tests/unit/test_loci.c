/**
 * @file test_loci.c
 * @brief Unit tests for LOCI emulation (Sprint 34y skeleton)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-06
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/io/loci.h"
#include "../../include/utils/logging.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  [%d] %-50s ", ++tests_run, #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { \
        printf("FAIL\n    %s:%d: expected %ld, got %ld\n", \
               __FILE__, __LINE__, _b, _a); \
        exit(1); \
    } \
} while(0)

/* ── lifecycle ───────────────────────────────────────────────── */

TEST(test_init_zeroes_state) {
    loci_t l; memset(&l, 0xFF, sizeof(l));
    ASSERT_TRUE(loci_init(&l));
    for (size_t i = 0; i < sizeof(l.regs); i++) ASSERT_EQ(l.regs[i], 0);
    ASSERT_EQ(l.active_op, 0);
    ASSERT_EQ(l.xstack_ptr, LOCI_XSTACK_SIZE);
}

TEST(test_addr_in_mia_filter) {
    ASSERT_TRUE(loci_addr_in_mia(0x03A0));
    ASSERT_TRUE(loci_addr_in_mia(0x03AF));
    ASSERT_TRUE(loci_addr_in_mia(0x03BF));
    ASSERT_TRUE(!loci_addr_in_mia(0x039F));
    ASSERT_TRUE(!loci_addr_in_mia(0x03C0));
}

/* ── bus interface — disabled ────────────────────────────────── */

TEST(test_disabled_read_returns_ff) {
    loci_t l; loci_init(&l);
    l.enabled = false;
    ASSERT_EQ(loci_read(&l, 0x03A0), 0xFF);
    ASSERT_EQ(loci_read(&l, 0x03AF), 0xFF);
}

TEST(test_disabled_write_is_noop) {
    loci_t l; loci_init(&l);
    l.enabled = false;
    loci_write(&l, 0x03AF, LOCI_OP_RNG_LRAND);
    ASSERT_EQ(l.regs[LOCI_REG_API_OP], 0);
    ASSERT_EQ(l.op_count[LOCI_OP_RNG_LRAND], 0);
}

/* ── register R/W ────────────────────────────────────────────── */

TEST(test_write_read_passthrough) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03A0, 0x42);
    ASSERT_EQ(loci_read(&l, 0x03A0), 0x42);
    loci_write(&l, 0x03B4, 0xAB);
    ASSERT_EQ(loci_read(&l, 0x03B4), 0xAB);
}

TEST(test_out_of_range_read_ff) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    ASSERT_EQ(loci_read(&l, 0x0399), 0xFF);
    ASSERT_EQ(loci_read(&l, 0x03C0), 0xFF);
}

/* ── API dispatch ────────────────────────────────────────────── */

TEST(test_api_op_dispatches_and_sets_enosys) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    /* errno should be ENOSYS */
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_ENOSYS);
    /* BUSY cleared after sync dispatch */
    ASSERT_TRUE((l.regs[LOCI_REG_BUSY] & 0x80) == 0);
    ASSERT_EQ(l.op_count[LOCI_OP_OPEN], 1);
}

TEST(test_api_op_none_does_not_dispatch) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_NONE);
    ASSERT_EQ(l.op_count[LOCI_OP_NONE], 0);
}

TEST(test_op_count_increments) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    for (int i = 0; i < 3; i++)
        loci_write(&l, 0x03AF, LOCI_OP_RNG_LRAND);
    ASSERT_EQ(l.op_count[LOCI_OP_RNG_LRAND], 3);
}

/* ── xstack ─────────────────────────────────────────────────── */

TEST(test_xstack_push_pop_roundtrip) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AC, 0x10);  /* push 0x10 */
    loci_write(&l, 0x03AC, 0x20);  /* push 0x20 */
    /* Read returns top of stack — 0x20 */
    ASSERT_EQ(loci_read(&l, 0x03AC), 0x20);
}

/* ── reset ──────────────────────────────────────────────────── */

TEST(test_reset_clears_state) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    loci_write(&l, 0x03A0, 0x55);
    loci_reset(&l);
    ASSERT_EQ(l.regs[LOCI_REG_API_ERRNO_LO], 0);
    ASSERT_EQ(l.regs[0], 0);
    ASSERT_EQ(l.active_op, 0);
    ASSERT_EQ(l.xstack_ptr, LOCI_XSTACK_SIZE);
}

int main(void) {
    log_init(LOG_LEVEL_ERROR);
    printf("\n");
    printf("===========================================================\n");
    printf("  Phosphoric LOCI Emulation Tests (Sprint 34y skeleton)\n");
    printf("===========================================================\n\n");

    RUN(test_init_zeroes_state);
    RUN(test_addr_in_mia_filter);
    RUN(test_disabled_read_returns_ff);
    RUN(test_disabled_write_is_noop);
    RUN(test_write_read_passthrough);
    RUN(test_out_of_range_read_ff);
    RUN(test_api_op_dispatches_and_sets_enosys);
    RUN(test_api_op_none_does_not_dispatch);
    RUN(test_op_count_increments);
    RUN(test_xstack_push_pop_roundtrip);
    RUN(test_reset_clears_state);

    printf("\n===========================================================\n");
    printf("  Results: %d passed, %d failed (total: %d)\n",
           tests_passed, tests_run - tests_passed, tests_run);
    printf("===========================================================\n\n");
    return tests_passed == tests_run ? 0 : 1;
}
