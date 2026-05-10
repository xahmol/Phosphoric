/**
 * @file keyboard.h
 * @brief ORIC-1 keyboard matrix emulation with SDL2 mapping
 *
 * The ORIC keyboard is an 8-column x 8-row matrix.
 * Column select via VIA Port B bits 0-2 (74LS138 decoder).
 * Row data via PSG Port A (register 14), active low.
 * Scan result on VIA PB3 (ROM keyboard scanning method).
 *
 * Supports two mapping modes:
 * - QWERTY: positional mapping (SDL keycode → ORIC matrix position)
 * - AZERTY: symbolic mapping via SDL_TEXTINPUT (character → ORIC key combo)
 *
 * Key mapping table from Oricutron (Peter Gordon, GPL v2).
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef HAS_SDL2
#include <SDL2/SDL.h>
#endif

/** Keyboard layout selection */
typedef enum {
    ORIC_KB_QWERTY = 0,  /**< QWERTY positional layout */
    ORIC_KB_AZERTY        /**< Symbolic mapping (AZERTY/any layout via TEXTINPUT) */
} oric_kb_layout_t;

/** Max simultaneous key presses tracked in symbolic mode */
#define ORIC_KB_MAX_PRESSED 16

/**
 * @brief ORIC keyboard matrix (8 columns x 8 rows)
 * Active low: 0xFF = no keys, bit clear = key pressed.
 */
typedef struct oric_keyboard_s {
    uint8_t matrix[8];
    oric_kb_layout_t layout;
#ifdef HAS_SDL2
    /* Symbolic mode: track pressed keys for proper release */
    struct {
        uint32_t scancode;
        int8_t row;
        int8_t col;
        bool shift;     /* This key needs ORIC Shift */
    } pressed[ORIC_KB_MAX_PRESSED];
    int pressed_count;
    uint32_t pending_scancode;  /* Last KEYDOWN scancode awaiting TEXTINPUT */
    bool has_pending;
#endif
} oric_keyboard_t;

void oric_keyboard_init(oric_keyboard_t* kb);
void oric_keyboard_reset(oric_keyboard_t* kb);
void oric_keyboard_set_layout(oric_keyboard_t* kb, oric_kb_layout_t layout);

/**
 * @brief Press a key by ASCII character (for automated input / --type-keys)
 *
 * Sets the appropriate matrix bits for the given character.
 * Call oric_keyboard_release_all() after a delay to release.
 *
 * @return true if the character was mapped
 */
bool oric_keyboard_press_char(oric_keyboard_t* kb, char c);

/**
 * @brief Release all keys (clear matrix to 0xFF)
 */
void oric_keyboard_release_all(oric_keyboard_t* kb);

#ifdef HAS_SDL2
/**
 * @brief Handle SDL2 event and update ORIC keyboard matrix
 *
 * In QWERTY mode: handles SDL_KEYDOWN/SDL_KEYUP (positional mapping).
 * In AZERTY mode: handles SDL_TEXTINPUT + SDL_KEYDOWN/UP (symbolic mapping).
 *
 * @return true if the event was mapped to an ORIC key
 */
bool oric_keyboard_handle_sdl_event(oric_keyboard_t* kb, const SDL_Event* event);
#endif

#endif /* KEYBOARD_H */
