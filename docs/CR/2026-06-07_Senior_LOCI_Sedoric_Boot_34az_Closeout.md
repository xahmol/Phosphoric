# Closeout senior review — Sedoric boot via LOCI résolu (sprint 34az)

**Date** : 2026-06-07
**Version livrée** : v1.16.54-alpha
**Référence amont** : `2026-06-07_Senior_LOCI_Sedoric_Boot_34ay.md` + réponse senior

---

## 1. TL;DR

Sedoric V4.0 boote complètement via LOCI. Menu DOS affiché identique au
boot natif Microdisc, prompt « Faites votre choix : » prêt à recevoir
input clavier. 484 tests PASS.

Bug livré par le senior, fix en 4 lignes.

---

## 2. Suivi des hypothèses senior

| Hypothèse senior | Statut après vérification |
|------------------|---------------------------|
| H1 Read Multiple manquant : FAUX | ✅ Confirmé via `disk.c:292` + `169-184` |
| H3 interleave naïf : FAUX | ✅ Confirmé via `sedoric.c:74-75` (déinterleave par address mark ID) |
| Pont CTRL ne propage pas SIDE/DRIVE | ❌ Réfuté pour mon code 34ax — propagation déjà présente (`loci.c:1935-1939`) |
| Test différentiel native Microdisc | ✅ Exécuté : boot **complet** en natif → bug 100 % côté pont LOCI |

Le senior avait le **bon framework de diagnostic** (test différentiel + audit
des callbacks CTRL), même si le delta précis n'était pas SIDE/DRIVE.

---

## 3. Bug réel : ROMDIS hardcodé true

### Code fautif (sprint 34ax, `loci.c:1944` avant fix)

```c
if (loci->dsk_sync_overlay) {
    loci->dsk_sync_overlay(loci->dsk_bus_ctx, true /* ← hardcodé */, diskrom);
}
```

### Référence native (`microdisc.c:127`)

```c
md->romdis = (value & MICRODISC_CTRL_ROMDIS) == 0;  /* Bit 1: 0=ROM disabled */
```

Puis `main.c:657` :
```c
emu->memory.basic_rom_disabled = emu->microdisc.romdis;  /* dynamique */
```

### Pourquoi le stage 2 bloquait

Sedoric V4.0, après chargement de son noyau en RAM overlay
($C000-$DFFF + $E000-$FFFF), écrit dans CTRL `(value & 0x02) == 1`
(ROMDIS désactivé) à chaque fois qu'il veut appeler une routine BASIC
ROM ($C000-$DFFF) — par exemple pour le formattage écran ou la gestion
clavier. Tout en laissant EPROM=0 pour garder le ROM Microdisc accessible
en $E000.

Avec mon hardcode `basic_rom_disabled=true`, le BASIC ROM ne revenait
**jamais** : chaque JSR vers $C0xx tombait dans la RAM overlay (lue avant
toute écriture donc 0x00) → boucle morte à $04F7.

### Fix livré (`loci.c:1929-1944`)

```c
bool diskrom = (value & 0x80) == 0;   /* bit 7 active-low : EPROM */
bool romdis  = (value & 0x02) == 0;   /* bit 1 active-low : ROMDIS */
/* ... */
if (loci->dsk_sync_overlay) {
    loci->dsk_sync_overlay(loci->dsk_bus_ctx, romdis, diskrom);
}
```

4 lignes nettes, mirror exact du code natif.

---

## 4. Régression sprint 34ax éclairée

Le commentaire que j'avais laissé dans le code 34ax disait :

> *« basic_rom_disabled reste persistant à true depuis le rom_swap_cb
> (sinon le ROM Microdisc en cours d'exécution disparaît du mapping). »*

C'était une mauvaise conclusion : le ROM Microdisc en $E000-$FFFF est
contrôlé par **`overlay_active`** (bit EPROM), pas par
`basic_rom_disabled` (qui contrôle $C000-$DFFF). Le sprint 34ax avait été
fait sur un mauvais reproducer (probablement l'instant T où Sedoric n'avait
pas encore basculé son DOS en overlay). Le fix dynamique 34az corrige les
deux cas car bit EPROM et bit ROMDIS sont indépendants par construction.

---

## 5. Validation E2E (test différentiel commitable)

Commande native (référence) :
```bash
./oric1-emu -r roms/basic11b.rom --disk-rom roms/microdis.rom \
    -d /tmp/SEDO40U.DSK -c 40000000 \
    --dump-ram-at 35000000:/tmp/native.bin
```

Commande LOCI :
```bash
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_demo.img \
    --keyboard azerty \
    --type-keys '15000000:\p3a\p2 \p2 \p2\e\p9\p9\p9' \
    -c 40000000 --dump-ram-at 35000000:/tmp/sedoric.bin
```

Les deux dumps RAM affichent l'écran texte $BB80 identique (10 entrées de
menu, prompt clavier). C'est le candidat naturel pour un futur
`test-loci-sedoric` E2E si le périmètre légal du dump SEDO40U.DSK est
clarifié, ou via un mini disque de boot clean-room (suggestion senior Q2).

---

## 6. Questions ouvertes résiduelles (suite à la réponse senior)

| Q | Position retenue |
|---|------------------|
| Q1 Bascule `microdisc_t` complet | **Reportée** — l'option est solide architecturalement (élimine la classe entière de bugs CTRL), mais le bridge `fdc_t` actuel marche bout-en-bout et le refactor coûterait ~200 LOC. À reconsidérer si un autre bug CTRL ressort en mode écriture disque. |
| Q2 Loader Sedoric clean-room pour E2E commit | **Pris note** — à faire si on veut un test-loci-sedoric versionné. Pas urgent : la fixture locale + commande différentielle suffit pour le dev. |
| Q3 Read Multiple dans le core | Sans objet (déjà implémenté). |

---

## 7. Méta-leçon

Test différentiel **avant** instrumentation lourde : 1 commande, 90
secondes de wall-clock, et la cause est isolée. C'est la suggestion
senior la plus rentable de cette série. À ajouter au playbook E2E LOCI
pour les futurs bugs « boote stage 1 puis bloque ».

---

*Branch :* `sprint-34ay`
*Commits clés :* `f967224` (34ay INTRQ) + suivant (34az ROMDIS dynamique)
*Test différentiel :* `/tmp/native.bin` vs `/tmp/sedoric.bin` au cycle 35M
