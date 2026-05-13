# Réponse équipe Phosphoric — debug IRQ T1 user vector Asteroids

**De** : équipe Phosphoric (bmarty)
**Pour** : équipe Asteroids Oric‑1
**Date** : 2026-05-13
**Objet** : réponse à `phosphoric-irq-debug-request.md` — diagnostic du bug
IRQ + réponses techniques + feature requests
**Phosphoric** : `v1.16.13-alpha` (rendu scanline-par-scanline ULA-accurate)

---

## TL;DR

**Le bug est trouvé** : votre handler IRQ suppose que la ROM a déjà fait
`PHA / TXA PHA / TYA PHA` avant de céder la main. **Cette supposition est
fausse** sur Oric‑1 BASIC 1.0 ET sur Atmos BASIC 1.1 quand vous patchez
`$0228` (ou `$0244`).

Le CPU 6502 **n'empile que PC et P** sur IRQ. Le PHA/PLA des registres
A/X/Y est fait *à l'intérieur* du handler ROM (à `$EC0C` sur Oric‑1, à
`$EE22` sur Atmos). En patchant `$0228`/`$0244`, vous **bypassez
entièrement la ROM** — donc aucun PHA n'a eu lieu avant que votre handler
ne tourne.

Quand votre handler fait `PLA / TAY / PLA / TAX / PLA / RTI`, il dépile :
1. `PLA` → tire le flags P (et le met dans A — perdu)
2. `TAY` → Y = P
3. `PLA` → tire PC_low (dans A)
4. `TAX` → X = PC_low
5. `PLA` → tire PC_high (dans A) — **mais le RTI suivant attend P + PC_low + PC_high sur la pile !**
6. `RTI` → dépile **garbage** comme P, **vrai PC** comme low, **vrai SP** comme high

Le PC final est **complètement faux**, d'où le hang dans une zone aléatoire
du code Asteroids.

## Fix (3 lignes asm)

```asm
_irq_handler:
        pha                       ; ← AJOUT : push A
        txa                       ; ← AJOUT
        pha                       ; ← AJOUT : push X
        tya                       ; ← AJOUT
        pha                       ; ← AJOUT : push Y
        lda  VIA_IFR
        and  #$40
        beq  @not_t1
        lda  VIA_T1CL
        jsr  _sound_tick
        inc  _frame_cnt
@not_t1:
        pla                       ; pop Y
        tay
        pla                       ; pop X
        tax
        pla                       ; pop A
        rti
```

Le contrat est : **votre handler est le PREMIER code exécuté sur IRQ**
quand vous patchez `$0228` / `$0244`. À vous de sauver les registres
manuellement.

---

## Réponses détaillées

### Q1 — Dispatch IRQ user vector Oric‑1 BASIC 1.0

> *« la ROM BASIC 1.0 fait-elle bien un `JMP ($0228)` (indirect) ou un
> `JMP $0228` (direct) ? »*

**Ni l'un ni l'autre — c'est le CPU 6502 lui-même qui dispatch directement
vers `$0228`** via son vecteur d'interruption matériel.

Mécanisme exact :

1. À IRQ, le CPU lit l'adresse stockée dans `$FFFE` / `$FFFF` (en ROM).
2. Pour la ROM Oric‑1 BASIC 1.0 : `$FFFE = $28`, `$FFFF = $02` → CPU jump
   vers **`$0228`** (lecture little-endian).
3. Le CPU **exécute le code à `$0228`** (comme une instruction normale,
   pas un indirect JMP).
4. La ROM au boot écrit en RAM à `$0228` les 3 bytes `4C 03 EC` (= JMP
   `$EC03`), qui chaine vers le handler ROM en `$EC03`. Vérifié à la main :
   ```
   Phosphoric savestate Oric-1 boot $0228-022D: 4C 03 EC 4C 30 F4
   ```
5. Le handler ROM à `$EC03` est `JMP $ED09` (lui-même un trampoline), qui
   eventuellement atteint `$EC0C` où `PHA / TXA PHA / TYA PHA` est exécuté.

**Votre `4C XX YY` à `$0228` est correct** comme méthode d'installation.
C'est exactement ce que la ROM fait elle-même au boot.

**Same pour Atmos** : `$FFFE/F` → `$0244`. ROM écrit `4C 22 EE` à `$0244`
(JMP `$EE22`). Vérifié :
```
Phosphoric savestate Atmos boot $0244-024F: 4C 22 EE 4C B2 F8 40 ...
                                                         ^^ $024A=$40 (RTI)
```

Notez le `$40` à `$024A` — c'est le **chain hook user** (voir Q2 option B).

### Q2 — Save/restore registres par la ROM avant `$0228`

> *« Mon handler suppose que la ROM IRQ a déjà fait PHA/TXA-PHA/TYA-PHA
> avant de céder la main à `$0228`. »*

**Faux.** Le contrat dépend de **où** vous interceptez :

**Option A — Remplacer la ROM IRQ entièrement (votre choix actuel)**

Vous patchez `$0228` / `$0244`. Le CPU dispatch directement chez vous, **la
ROM n'est pas exécutée**. Aucun PHA. Vous devez sauver/restaurer A/X/Y
vous-même.

**Option B — Chaîner après la ROM (recommandé pour compatibilité)**

Sur Atmos, le handler ROM `$EE22` finit par `JMP $024A`. À `$024A` la ROM
place `40` (= RTI). En remplaçant `$024A` par `4C XX YY` (JMP `_user_handler`),
votre handler tourne **après** la ROM, avec A déjà push/pop (la ROM
fait `PHA` à `$EE22` puis `PLA` à `$EE30` avant de chaîner — donc à
l'entrée de `$024A` les registres sont **propres**, comme avant l'IRQ).

Désassemblage Atmos `$EE22` :
```
$EE22: 48              PHA            ; push A
$EE23: AD 0D 03        LDA $030D      ; VIA_IFR
$EE26: 29 40           AND #$40       ; T1 ?
$EE28: F0 06           BEQ $EE30      ; non → skip
$EE2A: 8D 0D 03        STA $030D      ; clear T1 flag
$EE2D: 20 34 EE        JSR $EE34      ; ROM IRQ sub-handler (PHA X,Y dedans)
$EE30: 68              PLA            ; pop A
$EE31: 4C 4A 02        JMP $024A      ; ← user chain hook
```

Donc sur Atmos, **patcher `$024A` au lieu de `$0244`** vous donne :
- T1 IRQ déjà clear par la ROM
- A intact (PHA/PLA déjà fait)
- X / Y non touchés (la ROM les sauve/restore *dans `$EE34`* seulement si
  elle s'en sert)

Sur Oric‑1, le chain hook équivalent n'est pas évident dans la ROM BASIC
1.0 (le handler `$EC03 → $ED09` n'expose pas un user vector RAM
documenté). Le plus simple sur Oric‑1 reste l'Option A avec PHA correct.

**Pour la portabilité Oric‑1 + Atmos, recommandation** : Option A avec
PHA propre dans votre handler. C'est portable, simple, 3 lignes.

### Q3 — Comportement multi-source IRQ

> *« La ROM utilise-t-elle elle-même des sources IRQ VIA (T2 ? CA1 ?)
> pour son scan clavier ou son clock interne ? »*

Oui — **et c'est probablement la deuxième source de votre hang**.

Au boot, le ROM Atmos active typiquement :
- **T1 IRQ** : 100 Hz pour clock interne et scan clavier (`IER bit 6 = 1`)
- Possiblement CA2/CA1 pour gestion printer ACK

Quand vous faites `LDA #$C0 STA $030E`, vous ENABLEZ T1 mais ne **touchez
pas les autres bits** déjà activés par la ROM (bit 7 = 1 dans `$C0` est le
master set/clear flag, bits 6 = T1 enable). Les autres sources restent
actives.

Si **CB1 IRQ** est encore active (e.g. pendant un CLOAD ou en idle), et
qu'un front CB1 arrive, votre handler tourne, voit IFR & $40 = 0 (pas T1),
saute à `@not_t1` sans clear → CB1 reste set → IRQ re-fire immédiatement →
**infinite IRQ loop**.

**Fix robuste** dans `_irq_install` :
```asm
        sei
        lda  #$7F                 ; DISABLE all VIA IRQ sources first
        sta  $030E                ; (bit 7=0 = clear, bits 0-6=1 = clear all)
        lda  #$7F                 ; clear all IFR flags
        sta  $030D                ; (write 1s clears flags)
        ; ... votre patch $0228/$0244 ...
        lda  #$C0                 ; ENABLE T1 only (bit 7=1 set, bit 6=1 T1)
        sta  $030E
        cli
        rts
```

Si malgré tout une IRQ non-T1 fire (par exemple à cause d'un side-effect
ROM), votre handler peut clear toutes les sources en sécurité avec :
```asm
        lda  #$7F
        sta  $030D                ; clear ALL IFR flags
```
avant le RTI.

### Q4 — Outillage Phosphoric pour debug IRQ

Excellentes demandes — toutes pertinentes. Roadmap proposée :

#### 4.1 — `--trace-irq FILE` (priorité haute, ~3 h de dev)

Logger chaque IRQ servie : cycle, PC d'entrée (= valeur lue depuis
$FFFE/F), source (IFR snapshot AVANT clear), PC de retour (après RTI).

Implémentation : hook dans `cpu_step` quand il détecte une IRQ pending,
émet une ligne au format :
```
<cycle> IRQ entry: PC_pre=$XXXX → PC_post=$0228 IFR=XX IER=XX
<cycle> IRQ exit:  PC_post=$XXXX (via RTI)
```

Sprint envisagé : `34o` ou `35a`.

#### 4.2 — `--dump-ram-at C:FILE` (priorité haute, ~1 h de dev)

Dump 64 K de RAM à un cycle donné, format binaire raw. Variante :
`--dump-ram-at C:ADDR:LEN:FILE` pour une plage spécifique (e.g. zero page
`00:00FF`).

Idéal pour vérifier `frame_cnt` à différents instants. Permet aussi de
suivre `$0228`, `$0244`, `$024A`.

Sprint envisagé : `34o`.

#### 4.3 — Cycle-based breakpoint (priorité moyenne, ~2 h)

Extension de `--break ADDR` : `--break ADDR --break-after-cycle C`.
Stoppe au premier `PC=ADDR` après `cycle >= C`.

#### 4.4 — Debugger interactif : commandes ZP / RAM dump (priorité basse)

Le debugger `--debug` a déjà `mem ADDR LEN` (cf. `src/debugger.c`). Pour
ZP : alias `zp` → `mem 0000 100` proposé.

#### Priorités

Je peux livrer **4.1 + 4.2** dans la semaine si tu confirmes que c'est
utile (avant 4.3 + 4.4).

---

## Reproductibilité

Bug reproduit en local avec `oric1-emu v1.16.13-alpha` :
- Sans le sprint IRQ : asteroids tourne (polling IFR)
- Avec le sprint IRQ + handler `PLA / TAY / PLA / TAX / PLA / RTI`
  sans PHA d'entrée : hang à PC dans la zone code asteroids, comme
  observé chez vous

## Sources hardware

Dispatch IRQ Oric ROM vérifié par :
- Désassemblage Atmos `basic11b.rom` à `$EE22` (commit Phosphoric `6c9d30b`,
  script Python dans la session de debug 2026-05-13)
- Désassemblage Oric‑1 `basic10.rom` à `$EC03` → `$ED09` → `$EC0C`
- Lecture savestate `$0228`/`$0244` après boot, confirme JMP en RAM
- Cross-référence avec Oricutron `via.c` et conventions documentées
  sur [defence-force.org](https://forum.defence-force.org/)

## Contacts

- Phosphoric : `/home/bmarty/Oric1` (commit `6c9d30b`, `EMU_VERSION
  1.16.13-alpha`)
- Asteroids : `/home/bmarty/Oric asteroids` (commit `8af36cd`)
- bmarty <bmarty@mailo.com>

---

**Résumé** : ajoutez `PHA / TXA / PHA / TYA / PHA` en début de
`_irq_handler` et `LDA #$7F STA $030E` en début de `_irq_install`
(disable all VIA IRQ sources avant d'activer T1). Les 2 deltas devraient
faire passer le sprint IRQ. Pour l'outillage, dis-moi si tu veux
prioritiser `--trace-irq` ou `--dump-ram-at`.
