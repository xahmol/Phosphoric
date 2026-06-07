/**
 * @file control.c
 * @brief IPC control mode for OricForge IDE integration (sprint 35a)
 *
 * Implements the --control protocol described in include/control.h.
 * Reuses debugger.c primitives where possible.
 */

#define _POSIX_C_SOURCE 200809L
#include "control.h"
#include "emulator.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "debugger.h"
#include "utils/logging.h"
#include "io/via6522.h"
#include "audio/audio.h"
#include "io/microdisc.h"
#include "io/acia6551.h"
#include "io/loci.h"
#include "storage/disk.h"
#include <sys/select.h>
#include <unistd.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ─── output helpers ───────────────────────────────────────────────
 * All protocol output goes to stdout, one record per line, then we
 * flush so the IDE observes traffic in real time. */

static void reply_ok(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("OK", stdout);
    if (fmt && *fmt) {
        fputc(' ', stdout);
        vfprintf(stdout, fmt, ap);
    }
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

static void reply_err(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("ERR ", stdout);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

static void emit_evt(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("EVT ", stdout);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

/* Sprint 35a freeze — non-blocking stdin check called from the main loop
 * once per frame while the CPU is running. Returns true if the client
 * sent `pause` and the loop should hand control back to the REPL.
 * Other commands during running are NOT queued: `quit` exits, anything
 * else is rejected with ERR busy. Trade-off: simpler semantics for the
 * IDE, no command races. */
bool control_poll_pause(emulator_t* emu) {
    if (!emu->control_mode) return false;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) return false;
    if (!FD_ISSET(STDIN_FILENO, &fds)) return false;

    char line[1024];
    if (!fgets(line, sizeof(line), stdin)) {
        emu->running = false;
        return true;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    if (n == 0) return false;

    /* Strip first token for comparison. */
    char tok[16] = {0};
    sscanf(line, "%15s", tok);
    if (strcmp(tok, "pause") == 0) {
        reply_ok("pc=%04X cycles=%llu",
                 emu->cpu.PC, (unsigned long long)emu->cpu.cycles);
        emu->control_async_pause_pending = true;
        return true;
    }
    if (strcmp(tok, "quit") == 0) {
        reply_ok("");
        emu->running = false;
        return true;
    }
    reply_err("busy: emulator running, only `pause`/`quit` allowed "
              "(received `%s`)", tok);
    return false;
}

void control_emit_ready(emulator_t* emu) {
    emit_evt("ready pc=%04X cycles=%llu version=%s",
             emu->cpu.PC, (unsigned long long)emu->cpu.cycles, EMU_VERSION);
}

void control_emit_stopped(emulator_t* emu, const char* reason) {
    emit_evt("stopped pc=%04X cycles=%llu reason=%s",
             emu->cpu.PC, (unsigned long long)emu->cpu.cycles,
             reason ? reason : "unknown");
}

/* ─── parsing helpers ──────────────────────────────────────────────
 * Accept hex with or without `$`/`0x` prefix, plus plain decimal when
 * unambiguous. The IDE side is well-defined, so we stay strict. */

static bool parse_hex(const char* s, uint32_t* out) {
    if (!s || !*s) return false;
    if (*s == '$') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_u16(const char* s, uint16_t* out) {
    uint32_t v;
    if (!parse_hex(s, &v) || v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}

static bool parse_u8(const char* s, uint8_t* out) {
    uint32_t v;
    if (!parse_hex(s, &v) || v > 0xFF) return false;
    *out = (uint8_t)v;
    return true;
}

/* ─── command handlers ─────────────────────────────────────────── */

static void cmd_regs(emulator_t* emu) {
    reply_ok("A=%02X X=%02X Y=%02X SP=%02X P=%02X PC=%04X cycles=%llu",
             emu->cpu.A, emu->cpu.X, emu->cpu.Y, emu->cpu.SP, emu->cpu.P,
             emu->cpu.PC, (unsigned long long)emu->cpu.cycles);
}

static void cmd_set(emulator_t* emu, const char* reg, const char* val) {
    if (!reg || !val) { reply_err("set: usage `set <reg> <val>`"); return; }
    uint32_t v;
    if (!parse_hex(val, &v)) { reply_err("set: bad value"); return; }
    /* Case-insensitive: A, X, Y, SP, P, PC. */
    if (strcasecmp(reg, "a")  == 0) emu->cpu.A  = (uint8_t)v;
    else if (strcasecmp(reg, "x")  == 0) emu->cpu.X  = (uint8_t)v;
    else if (strcasecmp(reg, "y")  == 0) emu->cpu.Y  = (uint8_t)v;
    else if (strcasecmp(reg, "sp") == 0) emu->cpu.SP = (uint8_t)v;
    else if (strcasecmp(reg, "p")  == 0) emu->cpu.P  = (uint8_t)v;
    else if (strcasecmp(reg, "pc") == 0) emu->cpu.PC = (uint16_t)v;
    else { reply_err("set: unknown reg `%s`", reg); return; }
    reply_ok("");
}

static void cmd_read(emulator_t* emu, const char* addr_s, const char* len_s) {
    uint16_t addr;
    uint32_t len;
    if (!parse_u16(addr_s, &addr) || !parse_hex(len_s, &len)) {
        reply_err("read: usage `read <addr> <len>`");
        return;
    }
    if (len > 4096) { reply_err("read: len > 4096"); return; }
    /* Build the reply : "OK <hex bytes>". stdio printf per byte is fine
     * at this scale; the IDE caller batches reads anyway. */
    fputs("OK", stdout);
    for (uint32_t i = 0; i < len; i++) {
        fprintf(stdout, " %02X",
                memory_read(&emu->memory, (uint16_t)(addr + i)));
    }
    fputc('\n', stdout);
    fflush(stdout);
}

static void cmd_write(emulator_t* emu, char* rest) {
    /* `write <addr> <b0> <b1> ...` — at least one byte required. */
    char* save;
    char* addr_s = strtok_r(rest, " \t", &save);
    uint16_t addr;
    if (!addr_s || !parse_u16(addr_s, &addr)) {
        reply_err("write: usage `write <addr> <byte>...`");
        return;
    }
    int n = 0;
    char* tok;
    while ((tok = strtok_r(NULL, " \t", &save)) != NULL) {
        uint8_t b;
        if (!parse_u8(tok, &b)) {
            reply_err("write: bad byte at offset %d", n);
            return;
        }
        memory_write(&emu->memory, (uint16_t)(addr + n), b);
        n++;
    }
    if (n == 0) { reply_err("write: no bytes given"); return; }
    reply_ok("count=%d", n);
}

static void cmd_break(emulator_t* emu, const char* addr_s) {
    uint16_t addr;
    if (!parse_u16(addr_s, &addr)) {
        reply_err("break: usage `break <addr>`");
        return;
    }
    int id = debugger_add_breakpoint(&emu->debugger, addr);
    if (id < 0) { reply_err("break: full or rejected"); return; }
    reply_ok("id=%d addr=%04X", id, addr);
}

static void cmd_unbreak(emulator_t* emu, const char* id_s) {
    if (!id_s) { reply_err("unbreak: usage `unbreak <id>`"); return; }
    int id = atoi(id_s);
    if (!debugger_remove_breakpoint(&emu->debugger, id)) {
        reply_err("unbreak: invalid id");
        return;
    }
    reply_ok("");
}

static void cmd_break_list(emulator_t* emu) {
    debugger_t* dbg = &emu->debugger;
    fputs("OK", stdout);
    for (int i = 0; i < dbg->num_breakpoints; i++) {
        fprintf(stdout, " id=%d:addr=%04X", i, dbg->breakpoints[i].addr);
    }
    fputc('\n', stdout);
    fflush(stdout);
}

static void cmd_reset(emulator_t* emu) {
    cpu_reset(&emu->cpu);
    reply_ok("pc=%04X", emu->cpu.PC);
}

/* Sprint 35a freeze — protocol version + capability list. Bumped whenever
 * an existing command or event changes shape (additive `caps=` extensions
 * do NOT bump the version). */
#define CONTROL_PROTO_VERSION 1
#define CONTROL_PROTO_CAPS    "step-out,peek,hello,async-pause"

static void cmd_hello(const char* arg1, const char* arg2) {
    (void)arg1; (void)arg2;
    reply_ok("server=phosphoric/%s proto=%d caps=%s",
             EMU_VERSION, CONTROL_PROTO_VERSION, CONTROL_PROTO_CAPS);
}

/* Sprint 35a freeze — `peek <subsystem>` exposes the per-device REPL
 * commands (via/psg/disk/acia/tape/loci) in a single-line key=value
 * format so the IDE can populate its inspectors without parsing
 * human-friendly output. Each branch emits one line. */
static void cmd_peek(emulator_t* emu, const char* sub) {
    if (!sub) { reply_err("peek: usage `peek <subsystem>`"); return; }
    if (strcmp(sub, "via") == 0) {
        via6522_t* v = &emu->via;
        reply_ok("ora=%02X orb=%02X ddra=%02X ddrb=%02X "
                 "t1c=%04X t1l=%04X t2c=%04X t2l=%02X "
                 "acr=%02X pcr=%02X ifr=%02X ier=%02X sr=%02X "
                 "t1_run=%d t2_run=%d",
                 v->ora, v->orb, v->ddra, v->ddrb,
                 v->t1_counter, v->t1_latch, v->t2_counter, v->t2_latch,
                 v->acr, v->pcr, v->ifr, v->ier, v->sr,
                 v->t1_running ? 1 : 0, v->t2_running ? 1 : 0);
    }
    else if (strcmp(sub, "psg") == 0) {
        ay3891x_t* p = &emu->psg;
        fputs("OK", stdout);
        for (int i = 0; i < 14; i++) fprintf(stdout, " r%d=%02X", i, p->registers[i]);
        fprintf(stdout, " env_period=%u env_shape=%u env_step=%u env_vol=%u",
                p->env_period, p->env_shape, p->env_step, p->env_volume);
        fputc('\n', stdout);
        fflush(stdout);
    }
    else if (strcmp(sub, "disk") == 0 || strcmp(sub, "fdc") == 0) {
        if (!emu->has_microdisc) { reply_err("disk: microdisc inactive"); return; }
        microdisc_t* md = &emu->microdisc;
        fdc_t* f = &md->fdc;
        reply_ok("ctrl=%02X intrq=%d drq=%d diskrom=%d romdis=%d intena=%d "
                 "drive=%d side=%d cmd=%02X status=%02X trk=%02X sec=%02X "
                 "data=%02X dir=%d c_trk=%02X c_sec=%02X cur_off=%04X "
                 "drives_mounted=%d%d%d%d",
                 md->status,
                 md->intrq == 0x00 ? 1 : 0, md->drq == 0x00 ? 1 : 0,
                 md->diskrom, md->romdis, md->intena, md->drive, md->side,
                 f->command, f->status, f->track, f->sector, f->data,
                 f->direction, f->c_track, f->c_sector, f->cur_offset,
                 md->disk_data[0] != NULL, md->disk_data[1] != NULL,
                 md->disk_data[2] != NULL, md->disk_data[3] != NULL);
    }
    else if (strcmp(sub, "acia") == 0 || strcmp(sub, "serial") == 0) {
        acia6551_t* a = &emu->acia;
        reply_ok("tdr=%02X rdr=%02X status=%02X cmd=%02X ctrl=%02X "
                 "framebits=%u baud=%u v23=%d tx_pending=%d rx_full=%d "
                 "irq_line=%d dcd=%d dsr=%d cts=%d rx_fifo_count=%d "
                 "rx_fifo_size=%d",
                 a->tdr, a->rdr, a->status, a->command, a->control,
                 a->framebits, a->baud_rate, a->v23_mode ? 1 : 0,
                 a->tx_pending ? 1 : 0, a->rx_full ? 1 : 0,
                 a->irq_line ? 1 : 0, a->dcd ? 1 : 0, a->dsr ? 1 : 0,
                 a->cts ? 1 : 0, a->rx_fifo_count, a->rx_fifo_size);
    }
    else if (strcmp(sub, "tape") == 0 || strcmp(sub, "cassette") == 0) {
        reply_ok("loaded=%d pos=%d len=%d sync_loop=%d cload_active=%d "
                 "fastload_pending=%d csave_active=%d",
                 emu->tape_loaded ? 1 : 0, emu->tapeoffs, emu->tapelen,
                 emu->tape_syncstack >= 0 ? 1 : 0,
                 emu->tape_readbyte_active ? 1 : 0,
                 emu->fastload_pending ? 1 : 0,
                 emu->csave_file != NULL ? 1 : 0);
    }
    else if (strcmp(sub, "loci") == 0) {
        if (!emu->has_loci) { reply_err("loci: inactive"); return; }
        loci_t* l = &emu->loci;
        uint16_t err = (uint16_t)l->regs[LOCI_REG_API_ERRNO_LO]
                     | ((uint16_t)l->regs[LOCI_REG_API_ERRNO_HI] << 8);
        int fd_n = 0, dir_n = 0, mnt_n = 0;
        for (int i = 0; i < LOCI_FD_MAX; i++) if (l->fd_kind[i]) fd_n++;
        for (int i = 0; i < LOCI_DIR_MAX; i++) if (l->dir_kind[i]) dir_n++;
        for (int i = 0; i < LOCI_MNT_MAX; i++) if (l->mnt_mounted[i]) mnt_n++;
        uint64_t total = 0;
        for (int i = 0; i < 256; i++) total += l->op_count[i];
        reply_ok("enabled=%d active_op=%02X errno=%u busy=%02X "
                 "ops_total=%llu xstack_used=%u/%d "
                 "fds_open=%d dirs_open=%d mounts=%d "
                 "tap_pos=%u tap_size=%u dsk_selected=%d boot_settings=%02X "
                 "sdimg=%d",
                 l->enabled ? 1 : 0, l->active_op, err, l->regs[LOCI_REG_BUSY],
                 (unsigned long long)total,
                 LOCI_XSTACK_SIZE - l->xstack_ptr, LOCI_XSTACK_SIZE,
                 fd_n, dir_n, mnt_n,
                 l->tap_counter, l->tap_size,
                 l->dsk_selected, l->boot_settings,
                 l->sdimg ? 1 : 0);
    }
    else {
        reply_err("peek: unknown subsystem `%s` "
                  "(via|psg|disk|acia|tape|loci)", sub);
    }
}

/* ─── main REPL loop ──────────────────────────────────────────── */

void control_repl(emulator_t* emu) {
    char line[1024];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        /* Strip trailing newline. */
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n == 0) continue;   /* blank line, ignore */

        /* First token = command. */
        char* save;
        char* cmd = strtok_r(line, " \t", &save);
        if (!cmd) continue;
        char* arg1 = strtok_r(NULL, " \t", &save);
        char* arg2 = strtok_r(NULL, " \t", &save);

        if (strcmp(cmd, "hello") == 0) {
            cmd_hello(arg1, arg2);
        }
        else if (strcmp(cmd, "peek") == 0) {
            cmd_peek(emu, arg1);
        }
        else if (strcmp(cmd, "regs") == 0) {
            cmd_regs(emu);
        }
        else if (strcmp(cmd, "set") == 0) {
            cmd_set(emu, arg1, arg2);
        }
        else if (strcmp(cmd, "read") == 0) {
            cmd_read(emu, arg1, arg2);
        }
        else if (strcmp(cmd, "write") == 0) {
            cmd_write(emu, save);
        }
        else if (strcmp(cmd, "break") == 0) {
            cmd_break(emu, arg1);
        }
        else if (strcmp(cmd, "unbreak") == 0) {
            cmd_unbreak(emu, arg1);
        }
        else if (strcmp(cmd, "break-list") == 0) {
            cmd_break_list(emu);
        }
        else if (strcmp(cmd, "reset") == 0) {
            cmd_reset(emu);
        }
        else if (strcmp(cmd, "pause") == 0) {
            /* The REPL is only re-entered when execution is already
             * stopped, so `pause` is informational. */
            reply_ok("pc=%04X cycles=%llu",
                     emu->cpu.PC, (unsigned long long)emu->cpu.cycles);
        }
        else if (strcmp(cmd, "step") == 0) {
            emu->debugger.step_mode = true;
            emu->debugger.active = false;
            reply_ok("");
            return;
        }
        else if (strcmp(cmd, "next") == 0) {
            uint8_t opc = memory_read(&emu->memory, emu->cpu.PC);
            if (opc == 0x20) {
                emu->debugger.temp_breakpoint = (uint16_t)(emu->cpu.PC + 3);
                emu->debugger.has_temp_breakpoint = true;
                emu->debugger.step_mode = false;
            } else {
                emu->debugger.step_mode = true;
            }
            emu->debugger.active = false;
            reply_ok("");
            return;
        }
        else if (strcmp(cmd, "step-out") == 0) {
            /* Sprint 35a freeze-time addition : peek the return address
             * from the current stack frame (push order : hi first then lo,
             * so JSR stores PC-1 with hi at $0100+SP+2 and lo at SP+1).
             * RTS adds +1 to land on the instruction after JSR. */
            uint16_t sp = (uint16_t)(0x0100 + emu->cpu.SP);
            uint8_t lo = memory_read(&emu->memory, (uint16_t)(sp + 1));
            uint8_t hi = memory_read(&emu->memory, (uint16_t)(sp + 2));
            uint16_t ret = (uint16_t)(((uint16_t)hi << 8) | lo) + 1;
            emu->debugger.temp_breakpoint = ret;
            emu->debugger.has_temp_breakpoint = true;
            emu->debugger.step_mode = false;
            emu->debugger.active = false;
            reply_ok("ret=%04X", ret);
            return;
        }
        else if (strcmp(cmd, "continue") == 0) {
            emu->debugger.step_mode = false;
            emu->debugger.active = false;
            reply_ok("");
            return;
        }
        else if (strcmp(cmd, "quit") == 0) {
            reply_ok("");
            emu->running = false;
            return;
        }
        else {
            reply_err("unknown command `%s`", cmd);
        }
    }
    /* EOF on stdin — treat as quit. */
    emu->running = false;
}
