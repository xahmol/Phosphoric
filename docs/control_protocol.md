# Phosphoric IPC Control Protocol (sprint 35a)

Activate with `--control`. The emulator speaks a line-based text protocol
on stdin/stdout. Logs go to **stderr** so stdout stays a clean channel.

## Message kinds

| Direction | Prefix | Purpose |
|-----------|--------|---------|
| CMD       | (none) | Client → emu : one command per line |
| REP       | `OK ` or `ERR ` | Emu → client : reply to a CMD |
| EVT       | `EVT ` | Emu → client : spontaneous (e.g. break hit) |

All values are space-separated `key=value` tokens. Hex numbers accept
`$XXXX`, `0xXXXX`, or bare `XXXX` (4 chars max for u16, 2 for u8).

## Lifecycle

1. Emulator boots → emits `EVT ready pc=… cycles=0 version=…`.
2. Emulator stops immediately (control mode implies `--debug`-style
   initial break) → emits `EVT stopped pc=… cycles=0 reason=break`.
3. Client sends commands. Synchronous commands reply with `OK` /
   `ERR` and stay paused. Resume commands (`step`, `next`, `continue`)
   reply `OK` then hand control back to the CPU.
4. When the CPU stops again (breakpoint hit, step terminated, watchpoint),
   emulator emits `EVT stopped …` and waits for the next command.

## Commands implemented in 35a (frozen)

| CMD | Reply | Notes |
|-----|-------|-------|
| `hello [client=… proto=…]` | `OK server=phosphoric/X.Y proto=N caps=…` | recommended at session start |
| `regs` | `OK A=XX X=XX Y=XX SP=XX P=XX PC=XXXX cycles=N` | snapshot |
| `set <reg> <val>` | `OK` | reg = A/X/Y/SP/P/PC |
| `read <addr> <len>` | `OK XX XX XX ...` | bulk hex bytes, len ≤ 4096 |
| `write <addr> <b0> <b1> ...` | `OK count=N` | at least one byte |
| `peek <subsystem>` | `OK k=v k=v ...` | sub = `via`/`psg`/`disk`/`acia`/`tape`/`loci` |
| `break <addr>` | `OK id=N addr=XXXX` | adds PC breakpoint |
| `unbreak <id>` | `OK` | removes by index |
| `break-list` | `OK id=N:addr=XXXX [id=… …]` | |
| `step` | `OK` then `EVT stopped reason=step` | single instruction |
| `next` | `OK` then `EVT stopped reason=…` | step-over JSR |
| `step-out` | `OK ret=XXXX` then `EVT stopped reason=break` | reads return addr from stack |
| `continue` | `OK` then `EVT stopped reason=break` on break hit | |
| `pause` | `OK pc=… cycles=…` then `EVT stopped reason=user` | works both when stopped AND while running |
| `reset` | `OK pc=…` | warm reset |
| `quit` | `OK` then process exits | |

## Async commands while running

Once a `continue` or `step` has been acknowledged, the CPU is running.
The stdin parser keeps polling once per frame (~20 ms latency). During
this window only two commands are accepted :

- `pause` → emu replies `OK pc=… cycles=…` then emits
  `EVT stopped pc=… cycles=… reason=user`. The REPL is now interactive
  again.
- `quit` → emu replies `OK` and exits.

Anything else returns `ERR busy: emulator running, only pause/quit
allowed (received <cmd>)`. The IDE should not queue : send `pause`,
wait for `EVT stopped`, then send the synchronous command.

## Events

| EVT | Fields | Trigger |
|-----|--------|---------|
| `ready` | `pc cycles version` | once at start-up |
| `stopped` | `pc cycles reason` | every transition CPU → REPL |

Reason values : `break` (PC breakpoint hit, including step-out / next
landing on the temporary breakpoint), `step` (single step terminated),
`user` (async `pause` while running), `watch` (watchpoint write hit,
sprint 35b).

## Example session

```
EVT ready pc=F88F cycles=0 version=1.16.72-alpha
EVT stopped pc=F88F cycles=0 reason=break
> regs
OK A=00 X=00 Y=00 SP=FD P=24 PC=F88F cycles=0
> read $F88F 4
OK A2 FF 9A 58
> break $F893
OK id=0 addr=F893
> continue
OK
EVT stopped pc=F893 cycles=6 reason=break
> quit
OK
```

## Future sprints

- **35b** : `watch <addr>` + `EVT stopped reason=watch`,
  `raster <line>` + `EVT stopped reason=raster`, `load-tap`, `load-rom`,
  `load-sym`. Async pause-while-running.
- **35c** : strict framing with line numbers and ACK sequences,
  server-side timeouts, exhaustive client reference in Python.

## Error handling

Unknown commands or malformed arguments return `ERR <message>`. The
client should treat any non-`OK`/`EVT` line on stdout as a protocol
violation (logs are on stderr — never on stdout).

## Stability

Protocol shape may evolve until the OricForge integration is shipped to
production. Tag the consumed version in your IDE setup so a future change
in command shape doesn't surprise you.
