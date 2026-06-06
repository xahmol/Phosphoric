/**
 * @file test_debugger.c
 * @brief Debugger module unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-24
 * @version 1.1.0-alpha
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "debugger.h"
#include "emulator.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-50s", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: expected 0x%X, got 0x%X\n", __FILE__, __LINE__, (unsigned)(b), (unsigned)(a)); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("FAIL\n    %s:%d: expected true\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_FALSE(x) do { \
    if ((x)) { \
        printf("FAIL\n    %s:%d: expected false\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 1: INIT STATE                                                */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_debugger_init) {
    debugger_t dbg;
    debugger_init(&dbg);

    ASSERT_FALSE(dbg.active);
    ASSERT_FALSE(dbg.step_mode);
    ASSERT_EQ(dbg.num_breakpoints, 0);
    ASSERT_EQ(dbg.num_watchpoints, 0);
    ASSERT_FALSE(dbg.watch_triggered);
    ASSERT_FALSE(dbg.has_temp_breakpoint);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 2: ADD/REMOVE BREAKPOINT                                     */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_add_remove_breakpoint) {
    debugger_t dbg;
    debugger_init(&dbg);

    /* Add breakpoints */
    int idx0 = debugger_add_breakpoint(&dbg, 0x1234);
    ASSERT_EQ(idx0, 0);
    ASSERT_EQ(dbg.num_breakpoints, 1);
    ASSERT_EQ(dbg.breakpoints[0].addr, 0x1234);

    int idx1 = debugger_add_breakpoint(&dbg, 0xF42D);
    ASSERT_EQ(idx1, 1);
    ASSERT_EQ(dbg.num_breakpoints, 2);

    /* Duplicate returns existing index */
    int idx_dup = debugger_add_breakpoint(&dbg, 0x1234);
    ASSERT_EQ(idx_dup, 0);
    ASSERT_EQ(dbg.num_breakpoints, 2);

    /* Remove first breakpoint */
    ASSERT_TRUE(debugger_remove_breakpoint(&dbg, 0));
    ASSERT_EQ(dbg.num_breakpoints, 1);
    ASSERT_EQ(dbg.breakpoints[0].addr, 0xF42D);

    /* Remove remaining */
    ASSERT_TRUE(debugger_remove_breakpoint(&dbg, 0));
    ASSERT_EQ(dbg.num_breakpoints, 0);

    /* Remove from empty list */
    ASSERT_FALSE(debugger_remove_breakpoint(&dbg, 0));
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 3: BREAKPOINT LIMIT                                          */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_breakpoint_limit) {
    debugger_t dbg;
    debugger_init(&dbg);

    /* Fill all slots */
    for (int i = 0; i < DEBUGGER_MAX_BREAKPOINTS; i++) {
        int idx = debugger_add_breakpoint(&dbg, (uint16_t)(0x1000 + i));
        ASSERT_EQ(idx, i);
    }
    ASSERT_EQ(dbg.num_breakpoints, DEBUGGER_MAX_BREAKPOINTS);

    /* Try to add one more - should fail */
    int idx = debugger_add_breakpoint(&dbg, 0xFFFF);
    ASSERT_EQ(idx, -1);
    ASSERT_EQ(dbg.num_breakpoints, DEBUGGER_MAX_BREAKPOINTS);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 4: ADD/REMOVE WATCHPOINT                                     */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_add_remove_watchpoint) {
    debugger_t dbg;
    debugger_init(&dbg);

    int idx0 = debugger_add_watchpoint(&dbg, 0x0400);
    ASSERT_EQ(idx0, 0);
    ASSERT_EQ(dbg.num_watchpoints, 1);
    ASSERT_EQ(dbg.watchpoints[0], 0x0400);

    int idx1 = debugger_add_watchpoint(&dbg, 0xBB80);
    ASSERT_EQ(idx1, 1);

    /* Duplicate */
    int idx_dup = debugger_add_watchpoint(&dbg, 0x0400);
    ASSERT_EQ(idx_dup, 0);
    ASSERT_EQ(dbg.num_watchpoints, 2);

    /* Remove */
    ASSERT_TRUE(debugger_remove_watchpoint(&dbg, 0));
    ASSERT_EQ(dbg.num_watchpoints, 1);
    ASSERT_EQ(dbg.watchpoints[0], 0xBB80);

    /* Watchpoint limit */
    debugger_init(&dbg);
    for (int i = 0; i < DEBUGGER_MAX_WATCHPOINTS; i++) {
        debugger_add_watchpoint(&dbg, (uint16_t)(0x200 + i));
    }
    int idx_full = debugger_add_watchpoint(&dbg, 0xFFFF);
    ASSERT_EQ(idx_full, -1);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 5: BREAKPOINT HIT                                            */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_breakpoint_hit) {
    debugger_t dbg;
    debugger_init(&dbg);

    debugger_add_breakpoint(&dbg, 0xF42D);

    ASSERT_TRUE(debugger_is_breakpoint(&dbg, 0xF42D));
    ASSERT_FALSE(debugger_is_breakpoint(&dbg, 0xF42E));
    ASSERT_FALSE(debugger_is_breakpoint(&dbg, 0x0000));
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 6: BREAKPOINT MISS                                           */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_breakpoint_miss) {
    debugger_t dbg;
    debugger_init(&dbg);

    /* No breakpoints set - nothing should match */
    ASSERT_FALSE(debugger_is_breakpoint(&dbg, 0x0000));
    ASSERT_FALSE(debugger_is_breakpoint(&dbg, 0xFFFF));
    ASSERT_FALSE(debugger_is_breakpoint(&dbg, 0xF42D));

    /* Add one breakpoint, check others don't match */
    debugger_add_breakpoint(&dbg, 0x1000);
    ASSERT_FALSE(debugger_is_breakpoint(&dbg, 0x1001));
    ASSERT_FALSE(debugger_is_breakpoint(&dbg, 0x0FFF));
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 7: STEP MODE                                                 */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_step_mode) {
    debugger_t dbg;
    debugger_init(&dbg);

    /* Use a real emulator_t struct */
    emulator_t fake_emu;
    memset(&fake_emu, 0, sizeof(fake_emu));
    memory_init(&fake_emu.memory);
    cpu_init(&fake_emu.cpu, &fake_emu.memory);
    fake_emu.cpu.PC = 0x1000;

    /* Without step mode, no break (no breakpoints) */
    ASSERT_FALSE(debugger_should_break(&dbg, &fake_emu));

    /* With step mode, always break */
    dbg.step_mode = true;
    ASSERT_TRUE(debugger_should_break(&dbg, &fake_emu));

    memory_cleanup(&fake_emu.memory);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 8: DISASSEMBLE AT PC                                         */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_disassemble_at_pc) {
    /* Setup CPU + memory with a known instruction */
    memory_t mem;
    cpu6502_t cpu;
    memory_init(&mem);
    cpu_init(&cpu, &mem);

    /* Write LDA #$42 at $1000 */
    memory_write(&mem, 0x1000, 0xA9);  /* LDA immediate */
    memory_write(&mem, 0x1001, 0x42);

    /* Write JMP $F42D at $1002 */
    memory_write(&mem, 0x1002, 0x4C);  /* JMP absolute */
    memory_write(&mem, 0x1003, 0x2D);
    memory_write(&mem, 0x1004, 0xF4);

    char buf[64];
    int bytes;

    /* Disassemble LDA #$42 */
    bytes = cpu_disassemble(&cpu, 0x1000, buf, sizeof(buf));
    ASSERT_EQ(bytes, 2);

    /* Disassemble JMP $F42D */
    bytes = cpu_disassemble(&cpu, 0x1002, buf, sizeof(buf));
    ASSERT_EQ(bytes, 3);

    memory_cleanup(&mem);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MAIN                                                               */
/* ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Debugger Unit Tests\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    RUN(test_debugger_init);
    RUN(test_add_remove_breakpoint);
    RUN(test_breakpoint_limit);
    RUN(test_add_remove_watchpoint);
    RUN(test_breakpoint_hit);
    RUN(test_breakpoint_miss);
    RUN(test_step_mode);
    RUN(test_disassemble_at_pc);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
