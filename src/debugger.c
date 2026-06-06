/**
 * @file debugger.c
 * @brief Interactive debugger for Phosphoric
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-24
 * @version 1.1.0-alpha
 *
 * REPL debugger with breakpoints, watchpoints, single-step,
 * memory dump, disassembly, register inspection, and more.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "debugger.h"
#include "emulator.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "io/via6522.h"
#include "audio/audio.h"

/* Parse an address argument: tries the symbol table first (case-insensitive),
 * then falls back to hex parsing. Returns true if recognised. */
static bool parse_addr(const emulator_t* emu, const char* s, uint16_t* out) {
    if (!s || !*s) return false;
    if (symbol_resolve(&emu->symbols, s, out)) return true;
    if (*s == '$') s++;
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s || v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  INIT                                                               */
/* ═══════════════════════════════════════════════════════════════════ */

void debugger_init(debugger_t* dbg) {
    memset(dbg, 0, sizeof(*dbg));
    dbg->active = false;
    dbg->step_mode = false;
    dbg->num_breakpoints = 0;
    dbg->num_watchpoints = 0;
    dbg->watch_triggered = false;
    dbg->has_temp_breakpoint = false;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  BREAKPOINT MANAGEMENT                                              */
/* ═══════════════════════════════════════════════════════════════════ */

int debugger_add_breakpoint(debugger_t* dbg, uint16_t addr) {
    if (dbg->num_breakpoints >= DEBUGGER_MAX_BREAKPOINTS)
        return -1;
    /* Check for duplicate */
    for (int i = 0; i < dbg->num_breakpoints; i++) {
        if (dbg->breakpoints[i] == addr)
            return i;
    }
    dbg->breakpoints[dbg->num_breakpoints] = addr;
    return dbg->num_breakpoints++;
}

bool debugger_remove_breakpoint(debugger_t* dbg, int index) {
    if (index < 0 || index >= dbg->num_breakpoints)
        return false;
    /* Shift remaining breakpoints down */
    for (int i = index; i < dbg->num_breakpoints - 1; i++) {
        dbg->breakpoints[i] = dbg->breakpoints[i + 1];
    }
    dbg->num_breakpoints--;
    return true;
}

bool debugger_is_breakpoint(const debugger_t* dbg, uint16_t pc) {
    for (int i = 0; i < dbg->num_breakpoints; i++) {
        if (dbg->breakpoints[i] == pc)
            return true;
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  WATCHPOINT MANAGEMENT                                              */
/* ═══════════════════════════════════════════════════════════════════ */

int debugger_add_watchpoint(debugger_t* dbg, uint16_t addr) {
    if (dbg->num_watchpoints >= DEBUGGER_MAX_WATCHPOINTS)
        return -1;
    /* Check for duplicate */
    for (int i = 0; i < dbg->num_watchpoints; i++) {
        if (dbg->watchpoints[i] == addr)
            return i;
    }
    dbg->watchpoints[dbg->num_watchpoints] = addr;
    return dbg->num_watchpoints++;
}

bool debugger_remove_watchpoint(debugger_t* dbg, int index) {
    if (index < 0 || index >= dbg->num_watchpoints)
        return false;
    for (int i = index; i < dbg->num_watchpoints - 1; i++) {
        dbg->watchpoints[i] = dbg->watchpoints[i + 1];
    }
    dbg->num_watchpoints--;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MEMORY TRACE CALLBACK (for watchpoints)                            */
/* ═══════════════════════════════════════════════════════════════════ */

/* Global pointer to active debugger (needed by trace callback) */
static debugger_t* g_trace_debugger = NULL;

static void watchpoint_trace_callback(uint16_t address, uint8_t value, mem_access_type_t type) {
    (void)value;
    if (type != MEM_WRITE || !g_trace_debugger)
        return;
    for (int i = 0; i < g_trace_debugger->num_watchpoints; i++) {
        if (g_trace_debugger->watchpoints[i] == address) {
            g_trace_debugger->watch_triggered = true;
            g_trace_debugger->watch_addr_hit = address;
            return;
        }
    }
}

void debugger_install_watchpoint_trace(debugger_t* dbg, emulator_t* emu) {
    g_trace_debugger = dbg;
    if (dbg->num_watchpoints > 0) {
        memory_set_trace(&emu->memory, true, watchpoint_trace_callback);
    } else {
        memory_set_trace(&emu->memory, false, NULL);
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  SHOULD BREAK CHECK                                                 */
/* ═══════════════════════════════════════════════════════════════════ */

bool debugger_should_break(debugger_t* dbg, emulator_t* emu) {
    uint16_t pc = emu->cpu.PC;

    /* Step mode: always break */
    if (dbg->step_mode)
        return true;

    /* Temporary breakpoint (step-over) */
    if (dbg->has_temp_breakpoint && pc == dbg->temp_breakpoint) {
        dbg->has_temp_breakpoint = false;
        return true;
    }

    /* PC breakpoint hit */
    if (debugger_is_breakpoint(dbg, pc))
        return true;

    /* Watchpoint triggered */
    if (dbg->watch_triggered) {
        printf("\n*** WATCHPOINT hit: write to $%04X ***\n", dbg->watch_addr_hit);
        dbg->watch_triggered = false;
        return true;
    }

    return false;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  DISPLAY HELPERS                                                    */
/* ═══════════════════════════════════════════════════════════════════ */

static void show_registers(emulator_t* emu) {
    char state[128];
    cpu_get_state_string(&emu->cpu, state, sizeof(state));
    const char* sym = symbol_lookup(&emu->symbols, emu->cpu.PC);
    if (sym) printf("%s  <%s>\n", state, sym);
    else     printf("%s\n", state);
}

static void show_disassembly(emulator_t* emu, uint16_t addr, int count) {
    for (int i = 0; i < count; i++) {
        char buf[64];
        int bytes = cpu_disassemble(&emu->cpu, addr, buf, sizeof(buf));
        const char* sym = symbol_lookup(&emu->symbols, addr);
        if (sym) printf("  %s:\n", sym);
        printf("  $%04X: ", addr);
        for (int b = 0; b < 3; b++) {
            if (b < bytes)
                printf("%02X ", memory_read(&emu->memory, (uint16_t)(addr + b)));
            else
                printf("   ");
        }
        printf(" %s", buf);
        if (addr == emu->cpu.PC)
            printf("  <---");
        printf("\n");
        addr = (uint16_t)(addr + bytes);
    }
}

static void show_memory_dump(emulator_t* emu, uint16_t addr, int len) {
    for (int offset = 0; offset < len; offset += 16) {
        printf("  $%04X: ", (uint16_t)(addr + offset));
        /* Hex */
        for (int i = 0; i < 16 && (offset + i) < len; i++) {
            printf("%02X ", memory_read(&emu->memory, (uint16_t)(addr + offset + i)));
        }
        /* Pad if last line is short */
        int remaining = len - offset;
        if (remaining < 16) {
            for (int i = remaining; i < 16; i++)
                printf("   ");
        }
        /* ASCII */
        printf(" |");
        for (int i = 0; i < 16 && (offset + i) < len; i++) {
            uint8_t c = memory_read(&emu->memory, (uint16_t)(addr + offset + i));
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        printf("|\n");
    }
}

static void show_stack(emulator_t* emu) {
    uint8_t sp = emu->cpu.SP;
    int depth = 0xFF - sp;
    if (depth <= 0) {
        printf("  Stack is empty (SP=$%02X)\n", sp);
        return;
    }
    if (depth > 32) depth = 32; /* Limit display */
    printf("  SP=$%02X, depth=%d bytes\n", sp, 0xFF - sp);
    printf("  $01%02X: ", (uint8_t)(sp + 1));
    for (int i = 1; i <= depth; i++) {
        printf("%02X ", memory_read(&emu->memory, (uint16_t)(0x0100 + sp + i)));
        if (i % 16 == 0 && i < depth)
            printf("\n         ");
    }
    printf("\n");
}

static void show_via_state(emulator_t* emu) {
    via6522_t* via = &emu->via;
    printf("  VIA 6522 State:\n");
    printf("    ORA=$%02X ORB=$%02X  IRA=$%02X IRB=$%02X\n",
           via->ora, via->orb, via->ira, via->irb);
    printf("    DDRA=$%02X DDRB=$%02X\n", via->ddra, via->ddrb);
    printf("    T1: counter=$%04X latch=$%04X running=%s\n",
           via->t1_counter, via->t1_latch, via->t1_running ? "yes" : "no");
    printf("    T2: counter=$%04X latch=$%02X running=%s\n",
           via->t2_counter, via->t2_latch, via->t2_running ? "yes" : "no");
    printf("    ACR=$%02X PCR=$%02X\n", via->acr, via->pcr);
    printf("    IFR=$%02X IER=$%02X  SR=$%02X\n", via->ifr, via->ier, via->sr);
    /* Decode ACR */
    printf("    ACR decode: T1=%s T2=%s SR=%d\n",
           (via->acr & 0x40) ? "free-run" : "one-shot",
           (via->acr & 0x20) ? "count-PB6" : "one-shot",
           (via->acr >> 2) & 0x07);
    /* IRQ status */
    printf("    IRQ: %s (IFR & IER = $%02X)\n",
           (via->ifr & 0x80) ? "ASSERTED" : "inactive",
           via->ifr & via->ier & 0x7F);
}

static void show_psg_state(emulator_t* emu) {
    ay3891x_t* psg = &emu->psg;
    printf("  AY-3-8910 PSG State:\n");
    printf("    Registers: ");
    for (int i = 0; i < 14; i++)
        printf("%02X ", psg->registers[i]);
    printf("\n");
    /* Decode tone periods */
    for (int ch = 0; ch < 3; ch++) {
        uint16_t period = psg->registers[ch * 2] | ((psg->registers[ch * 2 + 1] & 0x0F) << 8);
        uint8_t vol = psg->registers[8 + ch];
        bool env = (vol & 0x10) != 0;
        printf("    Chan %c: period=%4d vol=%s%d\n",
               'A' + ch, period, env ? "E" : "", vol & 0x0F);
    }
    /* Noise */
    printf("    Noise: period=%d\n", psg->registers[6] & 0x1F);
    /* Mixer */
    uint8_t mix = psg->registers[7];
    printf("    Mixer ($%02X): Tone=%c%c%c Noise=%c%c%c\n", mix,
           (mix & 0x01) ? '-' : 'A',
           (mix & 0x02) ? '-' : 'B',
           (mix & 0x04) ? '-' : 'C',
           (mix & 0x08) ? '-' : 'A',
           (mix & 0x10) ? '-' : 'B',
           (mix & 0x20) ? '-' : 'C');
    /* Envelope */
    printf("    Envelope: period=%d shape=%d step=%d vol=%d %s\n",
           psg->env_period, psg->env_shape, psg->env_step,
           psg->env_volume, psg->env_holding ? "(holding)" : "");
}

static void show_help(void) {
    printf("\n  Debugger Commands:\n");
    printf("  ─────────────────────────────────────────────────\n");
    printf("  s / step          Step 1 instruction\n");
    printf("  n / next          Step over (JSR → break at return)\n");
    printf("  c / continue      Continue execution\n");
    printf("  r / regs          Show CPU registers\n");
    printf("  d [addr] [n]      Disassemble (default: PC, 10)\n");
    printf("  m addr [len]      Memory dump hex+ASCII (default: 256)\n");
    printf("  b addr            Add PC breakpoint\n");
    printf("  b                 List all breakpoints\n");
    printf("  bd n              Delete breakpoint #n\n");
    printf("  w addr            Add write watchpoint\n");
    printf("  w                 List all watchpoints\n");
    printf("  wd n              Delete watchpoint #n\n");
    printf("  via               Show VIA 6522 state\n");
    printf("  psg               Show PSG AY-3-8910 state\n");
    printf("  stack             Show stack contents\n");
    printf("  set reg val       Set register (A,X,Y,SP,PC,P)\n");
    printf("  sym [name|addr]   List symbols / resolve name or address\n");
    printf("  (addr args accept symbol names if --symbols was loaded)\n");
    printf("  q / quit          Quit emulator\n");
    printf("  h / help          Show this help\n");
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  REPL COMMAND LOOP                                                  */
/* ═══════════════════════════════════════════════════════════════════ */

void debugger_repl(debugger_t* dbg, emulator_t* emu) {
    dbg->active = true;
    dbg->step_mode = false;

    /* Show current state on entry */
    printf("\n*** DEBUGGER BREAK at $%04X ***\n", emu->cpu.PC);
    show_registers(emu);
    show_disassembly(emu, emu->cpu.PC, 1);

    char line[256];
    while (dbg->active) {
        printf("dbg> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            /* EOF on stdin - quit */
            emu->running = false;
            dbg->active = false;
            break;
        }

        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip empty lines */
        if (len == 0)
            continue;

        /* Parse command */
        char cmd[32] = {0};
        char arg1[32] = {0};
        char arg2[32] = {0};
        sscanf(line, "%31s %31s %31s", cmd, arg1, arg2);

        /* ── STEP ───────────────────────────────────────── */
        if (strcmp(cmd, "s") == 0 || strcmp(cmd, "step") == 0) {
            dbg->step_mode = true;
            dbg->active = false;
            /* Execute one instruction and come back */
        }
        /* ── NEXT (step-over) ───────────────────────────── */
        else if (strcmp(cmd, "n") == 0 || strcmp(cmd, "next") == 0) {
            /* Check if current instruction is JSR ($20) */
            uint8_t opcode = memory_read(&emu->memory, emu->cpu.PC);
            if (opcode == 0x20) {
                /* JSR abs: set temp breakpoint at PC+3 */
                dbg->temp_breakpoint = (uint16_t)(emu->cpu.PC + 3);
                dbg->has_temp_breakpoint = true;
                dbg->step_mode = false;
            } else {
                /* Not JSR: just step */
                dbg->step_mode = true;
            }
            dbg->active = false;
        }
        /* ── CONTINUE ───────────────────────────────────── */
        else if (strcmp(cmd, "c") == 0 || strcmp(cmd, "continue") == 0) {
            dbg->step_mode = false;
            dbg->active = false;
        }
        /* ── REGISTERS ──────────────────────────────────── */
        else if (strcmp(cmd, "r") == 0 || strcmp(cmd, "regs") == 0) {
            show_registers(emu);
        }
        /* ── DISASSEMBLE ────────────────────────────────── */
        else if (strcmp(cmd, "sym") == 0) {
            if (!arg1[0]) {
                if (emu->symbols.count == 0) {
                    printf("  No symbols loaded (use --symbols FILE)\n");
                } else {
                    printf("  %d symbols loaded\n", emu->symbols.count);
                    int show = emu->symbols.count > 20 ? 20 : emu->symbols.count;
                    for (int i = 0; i < show; i++)
                        printf("    $%04X  %s\n",
                               emu->symbols.entries[i].addr,
                               emu->symbols.entries[i].name);
                    if (emu->symbols.count > show)
                        printf("    … (%d more)\n", emu->symbols.count - show);
                }
            } else {
                uint16_t addr;
                if (parse_addr(emu, arg1, &addr)) {
                    const char* s = symbol_lookup(&emu->symbols, addr);
                    if (s) printf("  $%04X = %s\n", addr, s);
                    else   printf("  $%04X = (no symbol)\n", addr);
                } else {
                    printf("  Unknown symbol: %s\n", arg1);
                }
            }
        }
        else if (strcmp(cmd, "d") == 0) {
            uint16_t addr = emu->cpu.PC;
            int count = 10;
            if (arg1[0]) {
                uint16_t a;
                if (parse_addr(emu, arg1, &a)) addr = a;
            }
            if (arg2[0])
                count = atoi(arg2);
            if (count < 1) count = 1;
            if (count > 100) count = 100;
            show_disassembly(emu, addr, count);
        }
        /* ── MEMORY DUMP ────────────────────────────────── */
        else if (strcmp(cmd, "m") == 0) {
            if (!arg1[0]) {
                printf("  Usage: m addr [len]\n");
            } else {
                uint16_t addr;
                if (!parse_addr(emu, arg1, &addr)) {
                    printf("  Unknown address/symbol: %s\n", arg1);
                    continue;
                }
                int len2 = 256;
                if (arg2[0])
                    len2 = (int)strtol(arg2, NULL, 0);
                if (len2 < 1) len2 = 1;
                if (len2 > 65536) len2 = 65536;
                show_memory_dump(emu, addr, len2);
            }
        }
        /* ── BREAKPOINT ─────────────────────────────────── */
        else if (strcmp(cmd, "b") == 0) {
            if (!arg1[0]) {
                /* List breakpoints */
                if (dbg->num_breakpoints == 0) {
                    printf("  No breakpoints set\n");
                } else {
                    printf("  Breakpoints:\n");
                    for (int i = 0; i < dbg->num_breakpoints; i++) {
                        const char* s = symbol_lookup(&emu->symbols, dbg->breakpoints[i]);
                        if (s) printf("    #%d: $%04X  %s\n", i, dbg->breakpoints[i], s);
                        else   printf("    #%d: $%04X\n", i, dbg->breakpoints[i]);
                    }
                }
            } else {
                uint16_t addr;
                if (!parse_addr(emu, arg1, &addr)) {
                    printf("  Unknown address/symbol: %s\n", arg1);
                    continue;
                }
                int idx = debugger_add_breakpoint(dbg, addr);
                if (idx >= 0)
                    printf("  Breakpoint #%d set at $%04X\n", idx, addr);
                else
                    printf("  Error: maximum breakpoints reached (%d)\n",
                           DEBUGGER_MAX_BREAKPOINTS);
            }
        }
        /* ── BREAKPOINT DELETE ──────────────────────────── */
        else if (strcmp(cmd, "bd") == 0) {
            if (!arg1[0]) {
                printf("  Usage: bd <index>\n");
            } else {
                int idx = atoi(arg1);
                if (debugger_remove_breakpoint(dbg, idx))
                    printf("  Breakpoint #%d removed\n", idx);
                else
                    printf("  Invalid breakpoint index\n");
            }
        }
        /* ── WATCHPOINT ─────────────────────────────────── */
        else if (strcmp(cmd, "w") == 0) {
            if (!arg1[0]) {
                /* List watchpoints */
                if (dbg->num_watchpoints == 0) {
                    printf("  No watchpoints set\n");
                } else {
                    printf("  Watchpoints (write):\n");
                    for (int i = 0; i < dbg->num_watchpoints; i++) {
                        const char* s = symbol_lookup(&emu->symbols, dbg->watchpoints[i]);
                        if (s) printf("    #%d: $%04X  %s\n", i, dbg->watchpoints[i], s);
                        else   printf("    #%d: $%04X\n", i, dbg->watchpoints[i]);
                    }
                }
            } else {
                uint16_t addr;
                if (!parse_addr(emu, arg1, &addr)) {
                    printf("  Unknown address/symbol: %s\n", arg1);
                    continue;
                }
                int idx = debugger_add_watchpoint(dbg, addr);
                if (idx >= 0) {
                    printf("  Watchpoint #%d set at $%04X (write)\n", idx, addr);
                    debugger_install_watchpoint_trace(dbg, emu);
                } else {
                    printf("  Error: maximum watchpoints reached (%d)\n",
                           DEBUGGER_MAX_WATCHPOINTS);
                }
            }
        }
        /* ── WATCHPOINT DELETE ──────────────────────────── */
        else if (strcmp(cmd, "wd") == 0) {
            if (!arg1[0]) {
                printf("  Usage: wd <index>\n");
            } else {
                int idx = atoi(arg1);
                if (debugger_remove_watchpoint(dbg, idx)) {
                    printf("  Watchpoint #%d removed\n", idx);
                    debugger_install_watchpoint_trace(dbg, emu);
                } else {
                    printf("  Invalid watchpoint index\n");
                }
            }
        }
        /* ── VIA STATE ──────────────────────────────────── */
        else if (strcmp(cmd, "via") == 0) {
            show_via_state(emu);
        }
        /* ── PSG STATE ──────────────────────────────────── */
        else if (strcmp(cmd, "psg") == 0) {
            show_psg_state(emu);
        }
        /* ── STACK ──────────────────────────────────────── */
        else if (strcmp(cmd, "stack") == 0) {
            show_stack(emu);
        }
        /* ── SET REGISTER ───────────────────────────────── */
        else if (strcmp(cmd, "set") == 0) {
            if (!arg1[0] || !arg2[0]) {
                printf("  Usage: set <reg> <value>  (reg: A,X,Y,SP,PC,P)\n");
            } else {
                uint16_t val = (uint16_t)strtol(arg2, NULL, 16);
                /* Case-insensitive register name */
                char reg[8];
                strncpy(reg, arg1, sizeof(reg) - 1);
                reg[sizeof(reg) - 1] = '\0';
                for (int i = 0; reg[i]; i++) reg[i] = (char)toupper(reg[i]);

                if (strcmp(reg, "A") == 0) {
                    emu->cpu.A = (uint8_t)val;
                    printf("  A = $%02X\n", emu->cpu.A);
                } else if (strcmp(reg, "X") == 0) {
                    emu->cpu.X = (uint8_t)val;
                    printf("  X = $%02X\n", emu->cpu.X);
                } else if (strcmp(reg, "Y") == 0) {
                    emu->cpu.Y = (uint8_t)val;
                    printf("  Y = $%02X\n", emu->cpu.Y);
                } else if (strcmp(reg, "SP") == 0) {
                    emu->cpu.SP = (uint8_t)val;
                    printf("  SP = $%02X\n", emu->cpu.SP);
                } else if (strcmp(reg, "PC") == 0) {
                    emu->cpu.PC = val;
                    printf("  PC = $%04X\n", emu->cpu.PC);
                } else if (strcmp(reg, "P") == 0) {
                    emu->cpu.P = (uint8_t)val;
                    printf("  P = $%02X\n", emu->cpu.P);
                } else {
                    printf("  Unknown register: %s\n", arg1);
                }
            }
        }
        /* ── QUIT ───────────────────────────────────────── */
        else if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0) {
            emu->running = false;
            dbg->active = false;
        }
        /* ── HELP ───────────────────────────────────────── */
        else if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0) {
            show_help();
        }
        /* ── UNKNOWN ────────────────────────────────────── */
        else {
            printf("  Unknown command: '%s'. Type 'h' for help.\n", cmd);
        }
    }
}
