# LOCI ROM

LOCI ROM v0.3.0 by **Sodiumlightbaby** (sodiumlb), 2024.

- `locirom` — 16 KB raw ROM mapped at $C000-$FFFF (reset vector $FF29).
- `locirom.rp6502` — Same content with RP6502 header (`#!RP6502\n$C000 $400 $D3CA71B4\n`).

## Source

Downloaded from <https://github.com/sodiumlb/loci-rom/releases/tag/v0.3.0>.
Built from <https://github.com/sodiumlb/loci-rom> (cc65 toolchain).

## License

BSD-3-Clause, Copyright (c) 2024 Sodiumlightbaby — see source repository.

## Usage in Phosphoric

```bash
./oric1-emu -r roms/loci/locirom --loci
```

Requires Sprint 34y+ (`--loci` flag for MIA emulation at $03A0-$03BF).
Without `--loci`, the ROM boots but hangs polling the MIA registers.
