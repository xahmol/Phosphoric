/**
 * @file symbols.h
 * @brief Symbol table for debugger — load .sym / .lab / .sym65 files
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-06
 *
 * Auto-detected formats:
 *   Oricutron .sym   :  $E5BD .RDBYTE         (addr first)
 *                    :  RDBYTE = $E5BD        (name first, EQU)
 *   xa65 / VICE .lab :  al C E5BD .RDBYTE
 *   cc65 .sym65      :  rdbyte $E5BD          (lowercase, no prefix)
 *
 * Comments: lines starting with ';' or '#' are skipped.
 */

#ifndef SYMBOLS_H
#define SYMBOLS_H

#include <stdint.h>
#include <stdbool.h>

#define SYMBOL_MAX_COUNT 8192
#define SYMBOL_MAX_NAME  47

typedef struct {
    char     name[SYMBOL_MAX_NAME + 1];
    uint16_t addr;
} symbol_entry_t;

typedef struct {
    symbol_entry_t entries[SYMBOL_MAX_COUNT];
    int            count;
} symbol_table_t;

void        symbol_table_init(symbol_table_t* tbl);

/* Load symbols from FILE. Auto-detects format. Returns number of symbols
 * added (>=0) or -1 on file-open failure. Existing entries are preserved
 * (multiple loads accumulate). */
int         symbol_table_load(symbol_table_t* tbl, const char* path);

/* Lookup name from address. Returns NULL if not found.
 * If multiple symbols share an address, returns the first inserted. */
const char* symbol_lookup(const symbol_table_t* tbl, uint16_t addr);

/* Resolve a name to an address. Returns true on hit. Case-insensitive.
 * Accepts an optional leading '.' or '_' which is stripped. */
bool        symbol_resolve(const symbol_table_t* tbl, const char* name, uint16_t* out);

#endif /* SYMBOLS_H */
