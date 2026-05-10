# Bug — Scan clavier matriciel transposé (PSG reg 14 + VIA PB3)

**Statut :** ouvert
**Sévérité :** majeure (jeux scannant la VIA directement sont cassés)
**Date :** 2026-05-11
**Fichiers concernés :** `src/main.c` (lignes 329-336 et 358-382)

## Résumé

Le scan clavier matriciel (PSG reg 14 + VIA ORB[0:2] → PB3) lit la matrice en
transposée. Les callbacks `keyboard_matrix_read` (PSG Port A) et
`portb_read_callback` (VIA PB3) indexent `keyboard.matrix[]` par **colonne**
alors que `src/io/keyboard.c` la stocke indexée par **ligne** (cf. test
helper `KEY_IS_PRESSED` dans `tests/unit/test_keyboard.c:52`).

Résultat : le scan retourne le mauvais état pour quasiment toutes les touches.

## Symptôme observé

Programme de test : Asteroids Oric-1 (CC65 + asm 6502, scan VIA direct, ne
passe pas par la ROM).

Routine de scan (extrait `input.s`) :

```asm
;   - PSG reg 14 = $EF        (row 4 active, autres masquées)
;   - VIA ORB[0:2] = 5         (col 5 = SDLK_LEFT en QWERTY)
;   - lecture VIA ORB bit 3   → PB3 = 1 si touche pressée
        lda  #$EF
        ldy  #14
        jsr  psg_write
        ...
        lda  VIA_ORB
        and  #$F8
        ora  #5                ; col 5 = LEFT
        sta  VIA_ORB
        ...
        lda  VIA_ORB
        and  #$08              ; PB3
        beq  @no_left          ; PB3 = 0 → touche non pressée
```

Résultat sous Phosphoric (DDRA = $FF, DDRB = $F7, PSG reg 7 = $7F,
reg 14 = $EF) :

| Touche pressée         | Bit set dans key_state | Attendu                     |
|------------------------|------------------------|-----------------------------|
| ← LEFT (row 4, col 5)  | (aucun)                | bit 0                       |
| → RIGHT (row 4, col 7) | (aucun)                | bit 1                       |
| ↑ UP (row 4, col 3)    | (aucun)                | bit 2                       |
| SPACE (row 4, col 0)   | (aucun)                | bit 3                       |
| ↓ DOWN (row 4, col 6)  | (aucun)                | bit 4                       |
| R-SHIFT (row 7, col 4) | bit 1                  | aucun (col 4 jamais testée) |

R-SHIFT déclenche bit 1, qui correspond au test de col 7. Pattern exact d'une
matrice lue transposée : la touche réelle (row=7, col=4) apparaît à la
position (row=4, col=7) → détectée comme RIGHT.

## Analyse

`src/io/keyboard.c` stocke uniformément (QWERTY et symbolic AZERTY) :

```c
/* Convention : matrix[ROW], bit COL */
kb->matrix[row] &= ~(1 << col);   /* keypress */
kb->matrix[row] |=  (1 << col);   /* release  */
```

Cohérent avec le helper de test (`tests/unit/test_keyboard.c:52`) :

```c
#define KEY_IS_PRESSED(kb, row, col) (((kb).matrix[row] & (1 << (col))) == 0)
```

Mais `src/main.c` lit en transposé :

```c
/* main.c:329-336 — keyboard_matrix_read */
uint8_t col = emu->via.orb & 0x07;
uint8_t kbd = emu->keyboard.matrix[col];   /* ← matrix[col] : devrait
                                              recombiner les rows */

/* main.c:377 — portb_read_callback */
uint8_t pressed = (~emu->keyboard.matrix[col]) & (~reg14) & 0xFF;
/*                              ^^^ idem : matrix[col] au lieu de combiner
                                rows actives */
```

`reg14` étant un masque de rows (active-low), le AND avec `(~reg14)` n'a de
sens que si l'autre opérande est lui aussi indexé par row — ce qui n'est pas
le cas ici.

## Pourquoi non détecté par les tests existants

`tests/unit/test_keyboard.c` valide uniquement `oric_keyboard_handle_sdl_event`
(l'écriture dans `matrix[]`), pas le callback de scan utilisé par la VIA. Il
n'y a pas de test d'intégration qui presse une touche puis fait un scan complet
via reg 14 + ORB + lecture PB3.

La ROM BASIC 1.0/1.1 « marche » probablement parce qu'elle utilise un schéma
de scan qui rend la transposition invisible (par ex. reg14 = $00 ou lecture
de Port A du PSG plutôt que PB3). Mais tout programme qui scanne via PB3
(méthode VIA directe, requise pour le multi-touches d'un jeu) est cassé.

## Patch proposé

`portb_read_callback` (src/main.c:358) :

```c
uint8_t col = emu->via.orb & 0x07;
uint8_t col_mask = (uint8_t)(1 << col);
uint8_t reg14 = emu->psg.registers[14];

/* matrix[row] convention : bit `col` cleared iff (row, col) pressed.
 * For each row active in ~reg14, check if (row, col) is pressed. */
uint8_t any = 0;
for (int row = 0; row < 8; row++) {
    if (reg14 & (1 << row)) continue;          /* row inactive */
    if (!(emu->keyboard.matrix[row] & col_mask)) {
        any = 1;
        break;
    }
}
return any ? 0xFF : 0xF7;
```

`keyboard_matrix_read` (src/main.c:329) — doit retourner pour la colonne
sélectionnée l'état des 8 rows (bit r = état de (row=r, col=ORB[0:2])) :

```c
uint8_t col = emu->via.orb & 0x07;
uint8_t col_mask = (uint8_t)(1 << col);
uint8_t kbd = 0xFF;   /* active low */
for (int row = 0; row < 8; row++) {
    if (!(emu->keyboard.matrix[row] & col_mask))
        kbd &= (uint8_t)~(1 << row);
}
uint8_t joy = oric_joystick_read(&emu->joystick);
return kbd & joy;
```

## Test d'intégration recommandé

Pour empêcher la régression (à ajouter dans `tests/unit/test_keyboard.c` ou
nouveau `tests/integration/test_keyboard_scan.c`) :

```c
/* Press LEFT arrow (row 4, col 5), set reg14 = $EF (row 4 active),
 * select col 5 via ORB, read ORB → PB3 must be 1 */
oric_keyboard_t kb; oric_keyboard_init(&kb);
SDL_Event ev = { .type = SDL_KEYDOWN, .key.keysym.sym = SDLK_LEFT };
oric_keyboard_handle_sdl_event(&kb, &ev);

emu.psg.registers[14] = 0xEF;
emu.psg.registers[7]  = 0x7F;   /* Port A input */
emu.via.orb = (emu.via.orb & 0xF8) | 5;
uint8_t pb = portb_read_callback(&emu);
ASSERT_TRUE(pb & 0x08);          /* PB3 set → LEFT detected */
```

Couvrir aussi SPACE (col 0), UP (col 3), DOWN (col 6), RIGHT (col 7), et un
test négatif vérifiant qu'une touche d'une autre row n'est pas faussement
détectée.

## Environnement

- Repo : `/home/bmarty/Oric1` (Phosphoric)
- Branche : `main`
- Commit de référence : `a934cc8` (HEAD)
- Programme de test : projet Asteroids Oric-1 (CC65 + xa, scan VIA direct,
  voir `src/asm/input.s`)
- Émulation : `oric1-emu -m oric1 -r basic10.rom` en mode interactif SDL2
- Layout : QWERTY (positional) ; même symptôme attendu en AZERTY (même
  callback côté VIA)
