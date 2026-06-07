# Investigation : CSAVE BASIC 1.0 (ORIC-1) — `csave_end` jamais tiré

**Pour** : ingé sénior, suite de la review v1.16.44 → v1.16.45
**Date** : 2026-06-07
**Versions concernées** : tout depuis v1.16.43 (sprint 34aq, première
implémentation de la reconstruction TAP au `csave_end`)
**État** : root cause identifiée par désassemblée, fix proposé en
attente de ton OK

---

## 1. Symptôme

Sur Atmos (BASIC 1.1) :
```
CSAVE "T1" → CSAVE: built TAP T1.tap (30 bytes, prog $0501-$050E) ✓
CLOAD "T1" → programme chargé, LIST OK
```

Sur Oric-1 (BASIC 1.0) avec le même scénario :
```
CSAVE "T2" → fichier T2.tap créé mais 0 byte
CLOAD "T2" → "Searching ..." infini
```

---

## 2. Reachability des traps

Instrumentation `log_info` au tout début de chaque branche du
gestionnaire `tape_patches()` (commits non poussés) :

### Atmos (BASIC 1.1) — référence qui marche
```
TRACE: writeleader_entry $E75A (PC=$E75A SP=$F7)
CSAVE: saving to T1.tap
TRACE: putbyte_entry $E65E cnt=0   ← sync $24
TRACE: putbyte_entry $E65E cnt=1
TRACE: putbyte_entry $E65E cnt=2
TRACE: putbyte_entry $E65E cnt=3
TRACE: putbyte_entry $E65E cnt=4
TRACE: csave_end $E93C fired       ← ✓
CSAVE: built TAP T1.tap (30 bytes, ...)
```

### Oric-1 (BASIC 1.0) — qui ne marche pas
```
TRACE: writeleader_entry $E6BA (PC=$E6BA SP=$F7)
CSAVE: saving to T2.tap
TRACE: putbyte_entry $E5C6 cnt=0
TRACE: putbyte_entry $E5C6 cnt=1
TRACE: putbyte_entry $E5C6 cnt=2
TRACE: putbyte_entry $E5C6 cnt=3
TRACE: putbyte_entry $E5C6 cnt=4
<rien — csave_end $E7FE n'apparaît jamais>
```

Constat : writeleader et putbyte firent normalement. Le trap
`csave_end = $E7FE` jamais.

---

## 3. Désassemblée de la chaîne CSAVE Oric-1

### Routine CSAVE outer à `$E7DB`

```asm
$E7DB: A5 9A         LDA $9A         ; TXTTAB lo
$E7DD: A4 9B         LDY $9B         ; TXTTAB hi
$E7DF: 85 5F         STA $5F         ; copie TXTTAB → $5F/$60
$E7E1: 84 60         STY $60
$E7E3: A5 9C         LDA $9C         ; VARTAB lo
$E7E5: A4 9D         LDY $9D         ; VARTAB hi
$E7E7: 85 61         STA $61         ; copie VARTAB → $61/$62
$E7E9: 84 62         STY $62
$E7EB: 08            PHP
$E7EC: 20 25 E7      JSR $E725       ; ?? (setup)
$E7EF: 20 CA E6      JSR $E6CA       ; SetupTapeOutput (PSG init)
$E7F2: 20 7B E5      JSR $E57B       ; WriteFileHeader → écrit leader + sync + header + filename
$E7F5: 20 04 E8      JSR $E804       ; WriteDataBlock — ne revient pas !
$E7F8: 28            PLP
$E7F9: A6 A9         LDX $A9
$E7FB: E8            INX
$E7FC: F0 01         BEQ $E7FF
$E7FE: 60            RTS              ← csave_end actuel (jamais atteint)
$E7FF: 68            PLA
$E800: 68            PLA
$E801: 4C 6B C9      JMP $C96B
```

### La sub-routine `WriteDataBlock` à `$E804` (la « pierre angulaire »)

```asm
$E804: 20 63 E5      JSR $E563        ; écrit les données du programme
$E807: 20 39 F4      JSR $F439        ; cleanup ?
$E80A: 4C D0 EB      JMP $EBD0        ; ← JMP ! jamais retour !
```

**Cause racine** : `$E80A` est un `JMP`, pas un `RTS`. Le contrôle ne
revient JAMAIS au `JSR $E804` du caller à `$E7F5`, et donc le `RTS` à
`$E7FE` n'est jamais exécuté.

`$EBD0` est très probablement le point d'entrée du « Ready » prompt
BASIC (à confirmer par toi si tu as la table de symboles ORIC-1, mais
le pattern `JMP $EBD0` après cleanup est canonique pour les commandes
non-retournables comme NEW, CLOAD, CSAVE qui rebondissent direct au
main loop).

---

## 4. Comparaison avec Atmos (BASIC 1.1)

Sur Atmos, `csave_end = $E93C` fire normalement. Sans la table de
symboles complète, je n'ai pas confirmé que `$E93C` soit un `RTS` ou si
Atmos a une structure différente (peut-être un `RTS` propre, ou peut-être
ma trap fire sur une instruction intermédiaire et la routine continue).

Question pour toi : Atmos `CSAVE` revient-il vraiment proprement par RTS
chain, ou est-ce que ma trap à `$E93C` fire par hasard parce que c'est
une instruction commune dans le path et que `csave_byte_count` me
permet de catch le premier hit ?

---

## 5. Fixes envisagés

### Option A — Trap au JMP `$E80A`

```c
.csave_end = 0xE80A,   /* BASIC 1.0 — JMP $EBD0 marks end of CSAVE work */
```

Avantage : minimum d'invasivité, suit exactement la sémantique « fin
de tous les outputs CSAVE ». Mon code rebuilds le TAP avant que la JMP
exécute, donc le file et `tapebuf` sont prêts quand BASIC revient au
prompt.

Risque : pas vu. Le `JMP` est en fin de chain, pas en milieu de boucle.
Pas d'effet de bord sur les registres CPU (on intercepte avant la JMP
mais on laisse le PC inchangé sauf en sortant explicitement).

### Option B — Trap au `JSR $F439` à `$E807`

```c
.csave_end = 0xE807,
```

Tire après `JSR $E563` (écriture des données du programme) mais avant
le cleanup `JSR $F439`. Donne un point plus tôt, utile si on veut
encore manipuler la RAM avant que BASIC fasse du cleanup. Mais on
intercepte une instruction au milieu d'un cleanup → semantically less
clean qu'Option A.

### Option C — Étendre le mécanisme `csave_end` à plusieurs adresses

Le champ `rom_patches_t.csave_end` est un seul `uint16_t`. Si une ROM
finissait son CSAVE via plusieurs paths (par exemple JMP normal ou
RTS d'erreur), un seul point ne suffirait pas. Un `uint16_t csave_ends[4]`
+ comptage 0-terminé donnerait plus de flexibilité. **Pas justifié
pour 34at**, juste à garder en tête.

### Option D — Sourcer le buffer staging Oric-1

Découverte additionnelle pendant l'investigation : la routine
`$E57B WriteFileHeader` lit son header de buffer **`$5E..$66` (zero page)** :

```asm
$E57B: 20 BA E6      JSR $E6BA       ; writeleader (writes 6 leader bytes)
$E57E: A9 24         LDA #$24        ; sync
$E580: 20 C6 E5      JSR $E5C6       ; PutByte
$E583: A2 09         LDX #$09        ; X = 9
$E585: B5 5D         LDA $5D,X       ; ← header @ $5D+1..$5D+9 = $5E..$66
$E587: 20 C6 E5      JSR $E5C6       ; PutByte chaque byte
$E58A: CA            DEX
$E58B: D0 F8         BNE $E585
$E58D: B5 35         LDA $35,X       ; X=0 → filename @ $0035
$E58F: F0 06         BEQ +6
$E591: 20 C6 E5      JSR $E5C6       ; PutByte filename char
$E594: E8            INX
$E595: D0 F6         BNE $E58D
```

**Le buffer Oric-1 est en ZP à `$5E..$66`** (9 bytes, lus en reverse via
LDX#9 / DEX / BNE — même pattern qu'Atmos). Filename à `$0035` (comme
notre fallback historique).

Mapping mémoire → tape (X=9..1) :

| Adresse | Byte tape | Sens (présumé, à confirmer) |
|---------|-----------|------------------------------|
| `$66` | #1 | padding |
| `$65` | #2 | padding |
| `$64` | #3 | type |
| `$63` | #4 | auto-flag |
| `$62` | #5 | end_hi |
| `$61` | #6 | end_lo |
| `$60` | #7 | start_hi |
| `$5F` | #8 | start_lo |
| `$5E` | #9 | null sep |

Confirmation : la routine CSAVE outer à `$E7DB` copie `TXTTAB → $5F/$60`
et `VARTAB → $61/$62` juste avant d'appeler `$E57B`. Donc `$5F/$60 = start`
et `$61/$62 = end`. Ça matche le mapping ci-dessus.

→ On peut donner à Oric-1 un `csave_header_buf = 0x005E` propre dans
`rom_patches_t`, comme Atmos a `0x02A8`. Subsume la fallback
TXTTAB/VARTAB et marche aussi pour les CSAVE machine-code Oric-1
(`,A,E`) si le ROM les supporte.

---

## 6. Plan d'attaque proposé pour 34at

1. **Fix `csave_end` Oric-1** : `0xE7FE` → `0xE80A` (Option A)
2. **Ajouter `csave_header_buf = 0x005E`** pour `rom_patches_basic10`
3. **Tester round-trip BASIC 1.0** : `10 PRINT "HI"` → `CSAVE "T2"` →
   `NEW` → `CLOAD "T2"` → `LIST`
4. **Valider que TAP `T2.tap` matche format AIGLE.TAP** byte-pour-byte
   sur le header (sauf adresses)
5. **Régression Atmos** : s'assurer que rien ne casse

Questions ouvertes pour toi :

- Confirmation de `$EBD0` = main loop entry sur Oric-1 ? Si oui le
  pattern de fix par JMP-trap est bon. Sinon, peut-être faut-il un
  autre point.

- Sur Atmos, `$E93C` est-il vraiment le RTS de la routine CSAVE outer,
  ou est-ce que la même structure JMP-au-main-loop existe et que ma
  trap fire par accident sur une instruction au milieu ? Si c'est le
  cas, on devrait peut-être migrer Atmos vers la même stratégie
  (trap au JMP final, pas au RTS supposé) pour cohérence.

- Y a-t-il un cas où `BEQ $E7FF` à `$E7FC` (sur Oric-1) est pris
  (`X+1 == 0`) ? Si oui, le code à `$E7FF: PLA PLA JMP $C96B` est une
  voie de sortie alternative (erreur ?). Mon trap à `$E80A` la rate.
  Probablement pas grave (erreur ROM = pas de TAP à reconstruire) mais
  je préfère vérifier avec toi.

---

## 7. Métriques rapides

| Item | Valeur |
|------|--------|
| Versions impactées | Toutes depuis 1.16.43 (sprint 34aq) |
| Sévérité | Non-bloquant (Atmos marche, l'usage principal LOCI passe par Atmos), mais embarrassant pour les utilisateurs Oric-1 |
| Tests régression nécessaires | 470 tests existants + 1 nouveau test E2E Oric-1 round-trip |
| LOC fix estimé | < 10 (2 lignes dans `rom_patches_basic10` + tests) |
| Désassemblée nécessaire | Faite, attachée |

---

## 8. Annexes

### A.1 — `basic10.rom` bytes désassemblés

```
$E5C6  PutByte:
       85 2F           STA $2F          ; byte à écrire stocké en ZP
       8A 48           TXA PHA
       98 48           TYA PHA
       20 27 E6        JSR $E627
       ... (logique de parité bit-bang)
$E5F2  60              RTS              ; putbyte_end

$E6BA  WriteLeader (6 bytes) :
       A2 02           LDX #$02         ; 2 itérations externes
       A0 03           LDY #$03         ; 3 itérations internes
       A9 16           LDA #$16
       20 C6 E5        JSR $E5C6        ; PutByte
       88              DEY
       D0 F8           BNE $E6BE
       CA              DEX
       D0 F5           BNE $E6BE
$E6C9  60              RTS              ; writeleader_end

$E57B  WriteFileHeader :
       20 BA E6        JSR $E6BA        ; → 6 leaders
       A9 24           LDA #$24
       20 C6 E5        JSR $E5C6        ; → sync $24
       A2 09           LDX #$09
$E585: B5 5D           LDA $5D,X        ; ← BUFFER ZP $5E..$66
       20 C6 E5        JSR $E5C6
       CA              DEX
       D0 F8           BNE $E585
       B5 35           LDA $35,X        ; ← FILENAME $0035..
       F0 06           BEQ +6
       20 C6 E5        JSR $E5C6
       E8              INX
       D0 F6           BNE $E58D
       20 C6 E5        JSR $E5C6        ; sépaateur final ?

$E7DB  CSAVE outer routine :
       A5 9A           LDA $9A          ; TXTTAB → $5F/$60
       A4 9B           LDY $9B
       85 5F           STA $5F
       84 60           STY $60
       A5 9C           LDA $9C          ; VARTAB → $61/$62
       A4 9D           LDY $9D
       85 61           STA $61
       84 62           STY $62
       08              PHP
       20 25 E7        JSR $E725        ; ??
       20 CA E6        JSR $E6CA        ; SetupTapeOutput (PSG)
       20 7B E5        JSR $E57B        ; WriteFileHeader
       20 04 E8        JSR $E804        ; ↓ ne revient JAMAIS
$E7F8: 28              PLP              ; ← code mort sur le path normal
       A6 A9           LDX $A9
       E8              INX
       F0 01           BEQ $E7FF
$E7FE: 60              RTS              ; ← csave_end actuel, jamais atteint
$E7FF: 68 68           PLA PLA          ; chemin d'erreur ?
       4C 6B C9        JMP $C96B

$E804  WriteDataBlock :
       20 63 E5        JSR $E563        ; écrit data du programme
       20 39 F4        JSR $F439
$E80A: 4C D0 EB        JMP $EBD0        ; ← VRAI fin de CSAVE
```

### A.2 — Comparaison Atmos staging buffer

| Sub | Oric-1 (BASIC 1.0) | Atmos (BASIC 1.1) |
|-----|--------------------|---------------------|
| WriteFileHeader | `$E57B` | `$E607` |
| Header buffer base | `$005E` (ZP) | `$02A8` |
| Filename buffer base | `$0035` | `$027F` |
| Leader bytes count | 2×3 = 6 | (TBD vraisemblablement plus) |
| putbyte_entry | `$E5C6` | `$E65E` |
| putbyte_end | `$E5F2` | `$E68A` |
| csave_end (actuel, buggy) | `$E7FE` | `$E93C` (semble OK ?) |
| csave_end (proposé) | `$E80A` | (à confirmer) |

---

**Demande explicite** : ton OK sur l'Option A (`csave_end = $E80A`) +
la table buffer Oric-1 (`csave_header_buf = $005E`), et tes réponses
aux 3 questions ouvertes en section 6.

— Fin de l'investigation
