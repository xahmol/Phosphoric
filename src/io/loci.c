/**
 * @file loci.c
 * @brief LOCI emulation — skeleton + dispatcher (Sprint 34y)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-06
 *
 * Phase 1 implementation: MIA register file + API op dispatcher.
 * All API operations return LOCI_ENOSYS synchronously — subsequent sprints
 * implement the real handlers.
 *
 * Bus address layout (real LOCI hardware, extracted from sodiumlb/loci-firmware):
 *
 *   $03A0  CONS_FLAGS    bit 7 = console data avail, bit 6 = VSYNC ack
 *   $03A2  CONS_CHAR     console input character latch
 *   $03A4  RW0           DMA window 0 data byte
 *   $03A5  STEP0         DMA window 0 increment (signed)
 *   $03A8  RW1           DMA window 1 data byte
 *   $03A9  STEP1         DMA window 1 increment (signed)
 *   $03AC  API_STACK     xstack pointer (writes pop, reads push)
 *   $03AD  API_ERRNO_LO  16-bit errno
 *   $03AE  API_ERRNO_HI
 *   $03AF  API_OP        write triggers dispatch
 *   $03B0..$03B9         6502 injection slot (used by MIA for RAM bursts)
 *   $03B2.7              BUSY flag
 *   $03B4  API_A         return A
 *   $03B6  API_X         return X
 *   $03B8  API_SREG_LO   return SREG lo
 *   $03B9  API_SREG_HI   return SREG hi
 */

#include "io/loci.h"
#include "utils/logging.h"

#include <string.h>
#include <stdio.h>

/* ─── name lookup for diagnostics ───────────────────────────────── */

static const char* op_name(uint8_t op) {
    switch (op) {
        case LOCI_OP_PIX_XREG:        return "PIX_XREG";
        case LOCI_OP_CPU_PHI2:        return "CPU_PHI2";
        case LOCI_OP_OEM_CODEPAGE:    return "OEM_CODEPAGE";
        case LOCI_OP_RNG_LRAND:       return "RNG_LRAND";
        case LOCI_OP_STDIN_OPT:       return "STDIN_OPT";
        case LOCI_OP_CLOCK:           return "CLOCK";
        case LOCI_OP_CLK_GETRES:      return "CLK_GETRES";
        case LOCI_OP_CLK_GETTIME:     return "CLK_GETTIME";
        case LOCI_OP_CLK_SETTIME:     return "CLK_SETTIME";
        case LOCI_OP_OPEN:            return "OPEN";
        case LOCI_OP_CLOSE:           return "CLOSE";
        case LOCI_OP_READ_XSTACK:     return "READ_XSTACK";
        case LOCI_OP_READ_XRAM:       return "READ_XRAM";
        case LOCI_OP_WRITE_XSTACK:    return "WRITE_XSTACK";
        case LOCI_OP_WRITE_XRAM:      return "WRITE_XRAM";
        case LOCI_OP_LSEEK:           return "LSEEK";
        case LOCI_OP_UNLINK:          return "UNLINK";
        case LOCI_OP_RENAME:          return "RENAME";
        case LOCI_OP_OPENDIR:         return "OPENDIR";
        case LOCI_OP_CLOSEDIR:        return "CLOSEDIR";
        case LOCI_OP_READDIR:         return "READDIR";
        case LOCI_OP_MKDIR:           return "MKDIR";
        case LOCI_OP_GETCWD:          return "GETCWD";
        case LOCI_OP_MOUNT:           return "MOUNT";
        case LOCI_OP_UMOUNT:          return "UMOUNT";
        case LOCI_OP_TAP_SEEK:        return "TAP_SEEK";
        case LOCI_OP_TAP_TELL:        return "TAP_TELL";
        case LOCI_OP_TAP_READ_HEADER: return "TAP_READ_HEADER";
        case LOCI_OP_UNAME:           return "UNAME";
        case LOCI_OP_MIA_BOOT:        return "MIA_BOOT";
        case LOCI_OP_MAP_TUNE_TMAP:   return "MAP_TUNE_TMAP";
        case LOCI_OP_MAP_TUNE_TIOR:   return "MAP_TUNE_TIOR";
        case LOCI_OP_MAP_TUNE_TIOW:   return "MAP_TUNE_TIOW";
        case LOCI_OP_MAP_TUNE_TIOD:   return "MAP_TUNE_TIOD";
        case LOCI_OP_MAP_TUNE_TADR:   return "MAP_TUNE_TADR";
        default:                      return "?";
    }
}

/* ─── lifecycle ────────────────────────────────────────────────── */

bool loci_init(loci_t* loci) {
    if (!loci) return false;
    memset(loci, 0, sizeof(*loci));
    loci->xstack_ptr = LOCI_XSTACK_SIZE;   /* empty */
    return true;
}

void loci_reset(loci_t* loci) {
    if (!loci || !loci->enabled) return;
    memset(loci->regs, 0, sizeof(loci->regs));
    memset(loci->xstack, 0, sizeof(loci->xstack));
    loci->xstack_ptr = LOCI_XSTACK_SIZE;
    loci->active_op = 0;
}

void loci_cleanup(loci_t* loci) {
    if (!loci) return;
    /* Sprint 34y: no external resources to free. */
    (void)loci;
}

/* ─── errno / BUSY helpers ─────────────────────────────────────── */

static void set_errno(loci_t* loci, uint16_t e) {
    loci->regs[LOCI_REG_API_ERRNO_LO] = (uint8_t)(e & 0xFF);
    loci->regs[LOCI_REG_API_ERRNO_HI] = (uint8_t)(e >> 8);
}

static void set_busy(loci_t* loci, bool busy) {
    if (busy) loci->regs[LOCI_REG_BUSY] |=  0x80;
    else      loci->regs[LOCI_REG_BUSY] &= ~0x80;
}

/* ─── dispatch ─────────────────────────────────────────────────── */

/* Sprint 34y returns ENOSYS for every op. The actual handlers land in
 * later sprints. We still log each call for visibility. */
static void dispatch_op(loci_t* loci, uint8_t op) {
    loci->op_count[op]++;
    loci->active_op = op;
    log_debug("LOCI op $%02X (%s) — stubbed, returns ENOSYS", op, op_name(op));
    set_errno(loci, LOCI_ENOSYS);
    set_busy(loci, false);
    loci->active_op = 0;
}

/* ─── bus interface ────────────────────────────────────────────── */

uint8_t loci_read(loci_t* loci, uint16_t address) {
    if (!loci || !loci->enabled) return 0xFF;
    if (!loci_addr_in_mia(address)) return 0xFF;
    uint8_t off = (uint8_t)(address - LOCI_MIA_BASE);

    /* API_STACK: reading pushes from xstack (RP6502 semantics: 6502 reads
     * to retrieve a value pushed earlier by LOCI side, or pops what it
     * pushed). For Sprint 34y we simply return whatever is at the top
     * of the buffer without moving the pointer — enough to satisfy
     * detection probes from the LOCI ROM. */
    if (off == LOCI_REG_API_STACK) {
        if (loci->xstack_ptr >= LOCI_XSTACK_SIZE) return 0;
        return loci->xstack[loci->xstack_ptr];
    }

    return loci->regs[off];
}

void loci_write(loci_t* loci, uint16_t address, uint8_t value) {
    if (!loci || !loci->enabled) return;
    if (!loci_addr_in_mia(address)) return;
    uint8_t off = (uint8_t)(address - LOCI_MIA_BASE);

    /* API_STACK: 6502 writes push onto xstack (grows downward). */
    if (off == LOCI_REG_API_STACK) {
        if (loci->xstack_ptr > 0) {
            loci->xstack[--loci->xstack_ptr] = value;
        }
        loci->regs[off] = (uint8_t)loci->xstack_ptr;
        return;
    }

    loci->regs[off] = value;

    /* API_OP: writing here triggers a dispatch. The firmware sets BUSY
     * before the write so the polling Oric code waits for completion. */
    if (off == LOCI_REG_API_OP) {
        if (value != LOCI_OP_NONE && value != LOCI_OP_RESET_SENTINEL) {
            set_busy(loci, true);
            dispatch_op(loci, value);
        }
    }
}

void loci_task(loci_t* loci) {
    /* Sprint 34y: dispatch is synchronous in loci_write. No async work. */
    (void)loci;
}
