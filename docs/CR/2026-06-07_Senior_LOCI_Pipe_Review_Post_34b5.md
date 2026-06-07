# Senior review — Pipe LOCI/Sedoric post-validation E2E (sprints 34b0→34b5)

**Date** : 2026-06-07
**Périmètre** : `src/io/loci.c` (2276 LOC), `include/io/loci.h`, callbacks LOCI↔FDC dans `src/io/microdisc.c` + `src/storage/disk.c`, wiring `src/main.c`, `tests/integration/test_loci_sedoric_e2e.sh`.
**Référence amont** : `2026-06-07_Senior_LOCI_Sedoric_Boot_34az_Closeout.md` (fix ROMDIS dynamique).

---

## 1. TL;DR

Le pipe est **fonctionnellement solide** : 5 scénarios E2E byte-identiques avec le Microdisc natif, encapsulation propre du couple MIA/DSK/TAP, bridge `fdc_t` mutualisé. Aucun **P0** détecté.

Risques **résiduels modérés** (P1) : bug `op_readdir` POSIX (mauvais path pour `stat`), `strstr(p, "..")` trop laxiste, écritures DSK non-persistantes en mode raw, et `loci_overlay_buf` `static` (gestion de cycle de vie sale). Le fichier `loci.c` commence à atteindre la limite naturelle d'un seul TU et mériterait un découpage (file / dir / dsk / tap / boot).

Risque **résiduel léger** (P2) : couverture tests d'erreur et edge-cases timing FDC quasi-absente côté unit ; E2E couvre uniquement les chemins heureux.

---

## 2. Forces

1. **Séparation native ↔ LOCI bien tenue.** Le `fdc_t` cycle-accurate (`storage/disk.c`) est mutualisé via `dsk_set_disk`/`fdc_set_disk` ; ni `disk.c` ni `microdisc.c` ne référencent LOCI. Le pont est unidirectionnel (LOCI → FDC via callbacks DRQ/INTRQ symétriques aux callbacks Microdisc — comparer `loci.c:1776-1803` et `microdisc.c:23-48`). C'est ce qui a permis le fix 34az en 4 lignes.

2. **Contrat ABI du spin-window documenté et appliqué uniformément.** `api_install_blocked_stub` / `api_install_released_stub` / `api_return_*` (`loci.c:410-468`) encapsulent la sémantique self-modifying du firmware sodiumlb. Le commentaire à `loci.c:368-405` explique la sémantique BUSY overloadée sur l'opérande BVC — c'est rare et précieux. Tous les chemins de `dispatch_op` sortent via `api_return_*` ou `api_install_released_stub`, ce qui élimine la classe entière de hangs « JSR $03B0 sur zéro ».

3. **Symétrie POSIX/SDIMG correcte via `fd_kind[]`.** Le pattern « fds[i] non-NULL + fd_kind[i] discriminator » (`loci.c:184-197`, `868-869`, `944-945`) évite le type-punning des premiers sprints. Le cleanup mixte fonctionne sans leak.

4. **Fix 34az ROMDIS dynamique appliqué proprement.** `loci.c:1929-1948` mirrore exactement la logique `microdisc.c:115-137` (active-low bits, ordre d'application IRQ avant overlay). Le commentaire référence le bug origine, ce qui aide les futures lectures.

5. **Tests E2E différentiels.** `tests/integration/test_loci_sedoric_e2e.sh` est la bonne approche : comparer dump RAM native vs LOCI sur des régions stables ($BB80 écran, $0500 BASIC). Le skip gracieux des assets manquants permet le CI sans les ROMs propriétaires.

---

## 3. Risques résiduels

### P1 — Bug réel : `op_readdir` POSIX stat avec mauvais base path

**Fichier** : `loci.c:1549-1551`

```c
const char* base = loci->flash_root[0] ? loci->flash_root : ".";
snprintf(fullpath, sizeof(fullpath), "%s/%s", base, de->d_name);
if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) { ... }
```

**Scénario** : `opendir("sub/")` a ouvert un sous-répertoire (le `DIR*` pointe sur `sub/`), mais `op_readdir` reconstruit le chemin de stat avec `flash_root + de->d_name` — pas `sub/de->d_name`. Conséquences : (a) tous les fichiers dans un sous-rep sont classés non-DIR même s'ils sont DIR ; (b) `d_size` reporté = 0 sur entries inexistantes au niveau root. Pour Sedoric uniquement à la racine, **non observé** ; deviendra visible dès qu'une UI parcourt `OPENDIR /sub`.

**Mitigation** : stocker le path résolu dans un tableau parallèle `loci.dirs_path[LOCI_DIR_MAX][256]` lors de `op_opendir`, puis l'utiliser dans `op_readdir`. ~15 LOC.

### P1 — Path-traversal détecté par `strstr(p, "..")`

**Fichier** : `loci.c:713`

```c
if (strstr(p, "..")) return false;
```

**Scénario** : rejette les noms de fichiers légitimes contenant `..` (ex : `my..backup.dsk`, `a..b.tap`). Concurremment, ne détecte pas les variantes encodées URL ni les chemins absolus reconstitués post-strip de préfixe volume. Pas une faille de sécurité (l'attaquant contrôle déjà le `.img` SD), mais un risque **fonctionnel** : certains assets Sedoric ont des points multiples dans le nom.

**Mitigation** : tokeniser sur `/` et rejeter le composant exact `..` (et chaîne vide). 6 LOC.

### P1 — Écritures DSK non-persistées en mode raw / partiellement en mode MFM

**Fichier** : `loci.c:1825-1873` + `dsk_close` à `1875-1889`.

`dsk_open` charge l'image en RAM (`loci->dsk_image[drive]`), et `fdc_set_disk` y route les écritures. `dsk_fp` est ouvert `r+b` mais **jamais re-écrit** : ni dans `dsk_close`, ni dans `loci_cleanup`. Conséquence : les SAVE Sedoric sur disque attaché par `--loci-sdimg` traversent le path SDIMG (sync explicite à `main.c:1269`), donc OK pour le scénario E2E 34b2 ; mais un mount via `--loci-flash` perd les écritures à la fermeture.

**Scénario déclencheur** : `./oric1-emu --loci --loci-flash /tmp/myflash …`, mount un `.dsk`, SAVE depuis Sedoric, quitter. Le `.dsk` host est inchangé.

**Mitigation** : dans `dsk_close` (et `loci_cleanup`), si `dsk_image[i]` non-NULL et `dsk_fp[i]` ouvert en write, `fseek 0` + `fwrite(image, size)`. ~10 LOC. Alternative : flag dirty mis à jour par les writes FDC.

### P2 — `loci_overlay_buf` `static` dans `loci_rom_swap_cb`

**Fichier** : `src/main.c:364-367`

```c
static uint8_t* loci_overlay_buf = NULL;
if (loci_overlay_buf) { free(loci_overlay_buf); loci_overlay_buf = NULL; }
loci_overlay_buf = (uint8_t*)malloc((size_t)sz);
```

Le commentaire reconnaît la dette (« acceptable leak at shutdown »). Le storage `static` empêche aussi de tester en isolation (état résiduel inter-tests si jamais le main est appelé en boucle). Pas d'impact production immédiat.

**Mitigation** : déporter le buffer dans `emulator_t` (champ `loci_overlay_buf` + free dans `emulator_cleanup`).

### P2 — `op_tap_seek` clamp asymétrique

**Fichier** : `loci.c:1698-1699`

```c
if (loci->tap_size > 0 && pos >= loci->tap_size) {
    pos = loci->tap_size - 1;
}
```

Renvoie `pos = size-1` au lieu de `size` (le firmware autorise SEEK to EOF). Conséquence : un `TAP_SEEK(0xFFFFFFFF)` puis `TAP_TELL` reporte `size-1`, off-by-one vs firmware. Probable sans impact ROM observé.

### P2 — Pas de protection contre `xstack_ptr` corrompu

**Fichier** : `loci.c:2222-2227` (`API_STACK` write)

Le push décrémente `xstack_ptr` jusqu'à 0, après quoi il est silencieusement ignoré. Aucun errno remonté. Un `op_*` qui suppose un certain nombre de bytes peut alors lire des zéros (cas `op_open` avec path partiel). Le comportement firmware est identique, donc pas un bug, mais un piège pour le debug : symptôme = ENOENT alors que le 6502 « croyait » avoir pushé un path. Recommandation : `log_debug` quand `xstack_ptr == 0` au push.

---

## 4. Dette technique

- **`loci.c` = 2276 LOC dans un seul TU.** Sections nettement identifiables (`/* ─── ... ─── */`) — c'est une invitation à un split. Suggestion : `loci_core.c` (init/lifecycle/MIA register file + dispatch + xstack), `loci_fs.c` (open/close/read/write/lseek + opendir, POSIX + SDIMG dispatch), `loci_bus.c` (DSK + TAP register windows + callbacks FDC), `loci_boot.c` (MIA_BOOT + ROM resolution). ~1 sprint.

- **Duplications POSIX/SDIMG dans `op_write_xstack` (`loci.c:1084-1137`).** La logique de dépop+validate (35 LOC) est copiée. Une indirection via une mini-vtable `loci_fs_ops_t {open,close,read_n,write_n,lseek,unlink,rename}` est mentionnée dans `loci.h:232` (TODO vtable). Vrai dans : `op_unlink`, `op_rename`, `op_mkdir`, `op_opendir`, `op_closedir`, `op_readdir` (toutes ont un préambule sdimg + le path POSIX). ~150 LOC compressibles.

- **`dispatch_op` (`loci.c:2094-2144`) : 34 entrées de switch.** OK pour l'instant mais une table `static const struct { uint8_t op; void (*fn)(loci_t*); } ops[]` rendrait l'ajout d'une nouvelle op (et le tracing par nom) plus uniforme. Bénéfice limité.

- **Fonction `op_mount` (`loci.c:1312-1366`) : 55 LOC avec 3 chemins (SDIMG/POSIX, TAP/DSK), résolution path + auto-open + callback + persistance.** Mérite un découpage helper (`mount_resolve_path`, `mount_attach_backend`).

- **`derive_basic_rom_path` (`loci.c:1968-1973`) hardcode `basic11b.rom` / `basic10.rom`.** Si le user veut booter une ROM custom via MIA_BOOT sans monter `LOCI_MNT_ROM`, il échoue. À documenter ou à exposer via un setter.

- **TODO non résolu** : `loci.h:232` `TODO(vtable)` — actuellement 1 (POSIX/SDIMG/inused), pas un problème immédiat.

- **Conventions UTF-8 / commentaires français mélangés à l'anglais.** Cohérent dans les sprints récents, mais les sections plus anciennes (jusqu'à ~1200) sont 100 % anglais. Pas critique.

- **`op_uname` (`loci.c:1606-1611`) hardcode `release 1.16.27`.** Doit être un `EMU_VERSION` macro pour rester cohérent avec `include/emulator.h`. Risque : le 6502 voit une version obsolète si une ROM LOCI l'affiche.

- **Magic numbers `0x80` / `0x7F` pour DRQ/INTRQ** présents partout. Les `#define LOCI_DSK_STAT_*` existent ; les utiliser dans `loci_dsk_read` / `loci_fdc_*_intrq`.

---

## 5. Couverture de tests

### Bien couvert
- Chemin heureux boot Sedoric V4 (E2E 34b0).
- DIR catalogue (E2E 34b1).
- Round-trip SAVE/LOAD/RUN BASIC (E2E 34b2, 34b2b).
- TAP fast-load via LOCI + MIA_BOOT (E2E 34b5).
- xstack push/pop, MIA register file (`tests/unit/test_loci.c` probablement — non lu, mais référencé Makefile).

### Insuffisamment couvert
- **Chemins d'erreur ops API** : aucun test n'observe `errno=ENOSYS/EACCES/EIO/EBADF` côté 6502. Le faux-positif d'un ENOSYS silencieux passerait inaperçu.
- **Edge-cases timing FDC** : interleave inverse, multi-track Read sur un DSK à 18 secteurs (vs default 17). Le commentaire 34ay mentionne `interleave par address mark ID`, mais aucun test ne le confirme.
- **Conditions de course CTRL** : `op_mia_boot` charge `microdis.rom`, puis Sedoric écrit `$0314` avant le premier `RESTORE`. Un test différentiel sur l'ordre exact des appels `sync_overlay` (séquence attendue) éviterait une régression silencieuse.
- **Cleanup mixte FDC + SDIMG + flash_root** : pas de test pour la sortie propre en présence de fichiers ouverts SDIMG + dir ouverts + DSK monté.
- **Limite `xstack` saturée** : aucun test n'envoie 256 bytes de path.
- **Sécurité path traversal** : aucun test pour `..` / `0:..\..\etc\passwd`.
- **WRITE_XSTACK avec count = `LOCI_XSTACK_SIZE`** : edge case boundary.
- **`op_tap_read_header` sur TAP corrompu** (pas de sync mark, header tronqué).

---

## 6. Recommandations actionnables (sprint 34c)

Classées par ratio valeur/effort décroissant :

| # | Action | Effort | Valeur |
|---|--------|--------|--------|
| **R1** | Fix `op_readdir` POSIX : tracker path résolu par dir slot + utiliser le bon base pour `stat()` (`loci.c:1549-1551`). Ajouter un test unitaire avec sous-rep. | XS (~15 LOC + 1 test) | Évite un bug user-visible dès la 1re UI qui descend dans un sous-rep. |
| **R2** | Durcir `resolve_path` : tokeniser sur `/`, rejeter `..` exact (pas `strstr`). Ajouter 3 tests `EACCES` + 1 test « `my..file.tap` accepté » (`loci.c:702-718`). | XS (~10 LOC + 4 tests) | Vraie sécurité + débloque les noms légitimes. |
| **R3** | Persister les écritures DSK en mode raw : flush `image → fp` dans `dsk_close` + `loci_cleanup`. Ajouter test SAVE→quit→re-mount→LOAD via `--loci-flash` (`loci.c:1875-1889`). | S (~20 LOC + 1 test E2E) | Corrige une vraie perte de données silencieuse en mode flash. |
| **R4** | Découper `loci.c` en 4 TUs (`core / fs / bus / boot`). Pas d'extraction de vtable encore — juste un split mécanique avec headers privés. | M (~1 jour, refactor mécanique) | Le coût de maintenance baisse fortement, l'extraction de la vtable suivante devient triviale. |
| **R5** | Ajouter un test unitaire **chemins d'erreur** : `test_loci_errno.c` qui drive le dispatcher avec des entrées invalides et vérifie l'`errno` LOCI reporté (`$03AD/AE`). Couvre EBADF, EINVAL, ENOENT, EACCES sur 8-10 ops. | S (~150 LOC) | Filet de sécurité contre les régressions silencieuses des chemins d'erreur. |

**Hors scope sprint 34c, à acter** :
- Reprendre Q1 du closeout 34az (bascule complète vers `microdisc_t` au lieu de `fdc_t` bridge) si un autre bug CTRL survient en mode écriture.
- Mini disque Sedoric clean-room pour E2E versionné sans dépendance propriétaire.
- Documenter publiquement la sémantique du spin-window pour les futurs contributeurs (un README dans `src/io/loci/`).

---

*Reviewer : staff engineer, lecture intégrale `loci.c` + diff 34ax→34az + wiring main.c.*
*Aucun bug bloquant pour le merge des sprints 34b0-b5. R1/R2/R3 recommandés avant le prochain release tag.*
