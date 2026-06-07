# Réponse à ta review — preuves empiriques + 48 octets ROM

**Suite à** : `docs/CR/2026-06-07_Senior_BASIC10_CSAVE_Investigation.md`
**Date** : 2026-06-07
**Objet** : test discriminant exécuté + raw bytes pour vérification

---

## 1. Test PC-log core CPU (celui que tu proposais)

Patch posé directement dans `cpu_step()` avant le fetch de l'opcode,
inconditionnel — pas dans `tape_patches()`, pas de garde. Cible : log
sur `PC == $E7FE` et `PC == $E80A` pendant un `CSAVE "T2"` Oric-1.

```
$ make SDL2=1 && timeout 20 ./oric1-emu -r roms/basic10.rom \
    --type-keys '5000000:10 PRINT "HI"\n\p1CSAVE "T2"\n\p4'
```

Résultat brut (stderr) :
```
PCLOG: HIT $E80A (cycle=8000347 A=00 X=FF Y=00 SP=F9)
```

**Une seule ligne.** `$E7FE` n'apparaît jamais. `$E80A` apparaît une
fois, vers la fin du CSAVE (~8M cycles, soit 8 secondes wall-clock
après start, ce qui est cohérent avec la séquence `5M cycles → type
"CSAVE T2" → process CSAVE`).

C'est donc empirique : sur ma ROM, à ce moment précis, le path
exécuté ne passe PAS par `$E7FE`. Il passe par `$E80A`.

---

## 2. Identité de ma ROM

```
md5sum roms/basic10.rom roms/basic11b.rom
  ebe418ec8a6c85d5ac32956c9a96c179  roms/basic10.rom
  a330779c42ad7d0c4ac6ef9e92788ec6  roms/basic11b.rom
```

Tailles : 16384 bytes chacune (16 KB, mapping standard `$C000-$FFFF`).

Si tu peux comparer ces hashes à la ROM qu'Oricutron utilise dans son
`.pch`, ça nous dira si on parle de la même révision.

---

## 3. Les 48 octets `$E7DB..$E80A` (raw — à toi de désassembler)

Hex brut :
```
a59aa49b855f8460a59ca49d85618462082025e720cae6207be52004e828a6a9
e8f0016068684c6bc92063e52039f44c
```

Formaté :
```
$E7DB: A5 9A A4 9B 85 5F 84 60 A5 9C A4 9D 85 61 84 62
$E7EB: 08 20 25 E7 20 CA E6 20 7B E5 20 04 E8 28 A6 A9
$E7FB: E8 F0 01 60 68 68 4C 6B C9 20 63 E5 20 39 F4 4C
```

(Le `4C` à `$E80A` est l'opcode JMP ; les bytes suivants — `D0 EB`
selon mon listing — sont à `$E80B..$E80C`, hors fenêtre.)

---

## 4. Ce qui me tracasse dans ton point PHP/PLP

Tu écris :
> Pour que ce `PLP` s'exécute, le `JSR $E804` à `$E7F5` doit revenir
> par RTS. Une sous-routine appelée par JSR qui se terminerait par
> JMP $EBD0 ne revient jamais → PLP ne tourne pas → pile déséquilibrée.

Je suis d'accord sur la mécanique. **Mais** : si `$EBD0` est l'entrée
warm-start du BASIC main loop, la convention 6502 standard est qu'elle
commence par :
```asm
LDX #$FF
TXS                    ; reset stack
```
…ce qui défait toute la pile précédente. Une CSAVE qui termine par
`JMP $EBD0` aurait alors un déséquilibre intentionnel (le PHP push
est simplement abandonné). Ce pattern est canonique pour les commandes
non-retournables (CLOAD, NEW, RUN…) qui rebondissent au prompt.

Mon empirique (`$E80A` hit, `$E7FE` jamais) suggère que c'est ce qui
se passe sur ma ROM. Mais je veux ton avis : est-ce que `$EBD0` reset
bien la pile chez toi ? Si oui, l'argument PHP/PLP ne tient plus comme
preuve que `$E804` doit RTS.

---

## 5. Hypothèses possibles pour le décalage avec Oricutron

Plusieurs options non mutuellement exclusives :

1. **Révisions différentes de basic10.rom**. Il y a au moins deux ROMs
   BASIC 1.0 connues (« $00 ROM » et « $02 ROM », plus un dump
   intermédiaire qui flotte sur les sites collectionneur). Si mon
   MD5 ≠ celui d'Oricutron, on a probablement pas les mêmes adresses.

2. **Oricutron use `$E7FE` mais avec un effet de bord**. Comme tu le
   dis, leur `tape_stop_savepatch` ferme le `.tap` au point `$E7FE`.
   Mais si leur CSAVE flow passe AUSSI par `$E80A` AVANT, ils auraient
   peut-être un autre hook qui écrit les dernières données — ou leur
   `$E7FE` est touché par un path d'erreur/recovery qui chez moi
   n'arrive pas. Je n'ai pas leur source sous la main pour vérifier.

3. **Mon Oric-1 ROM est partiellement cassée / mal mappée**. Improbable
   (j'ai validé CLOAD, RUN, BASIC interactive tous OK), mais à exclure.

---

## 6. Ce que j'aimerais que tu confirmes

- Le MD5 d'Oricutron pour `basic10.rom` — match ou pas avec le mien.
- Le désassemblage propre des 48 octets ci-dessus, vu de tes yeux.
  Surtout : `$E80A` finit-il vraiment par `JMP $EBD0` ou est-ce que
  mon alignement décroche entre `$E7F5` et `$E80A` ?
- Si `$EBD0` est bien la warm-start BASIC sur Oric-1 (table de
  symboles) — ce qui validerait le pattern « JMP-au-main-loop avec
  stack reset ».
- Réaction à ta proposition « revenir à la capture putbyte façon
  Oricutron » : oui sur le principe, mais avant de la livrer je veux
  comprendre POURQUOI ma capture initiale produisait `FF FF` en tête
  de header — sinon je risque de réintroduire le même bug. Vois-tu un
  candidat (registre A pas encore set quand putbyte_entry fire,
  problème d'alignement de l'adresse de patch, ROM différente) ?

---

## 7. Ce que je ne ship PAS tant que tu n'as pas vu

- Le passage `csave_end → $E80A` est PRÊT mais en attente.
- Le `csave_header_buf = $005E` pour Oric-1 est PRÊT mais en attente
  (tu m'as alerté sur le piège : `$5F/$60` est probablement réutilisé
  comme pointeur de travail par la boucle data → si on le lit à
  csave_end, on a l'adresse de fin pas le start — je vais snapshot à
  l'entrée de `$E57B` plutôt).
- Pas de modif Atmos (ton point sur `$E93C` reçu).

Tout est sur ma branche `sprint-34at` locale, non poussé.

---

## 8. Petite note méta

J'apprécie que tu ailles vérifier dans la ROM réelle plutôt que
signer mes désassemblages. C'est aussi pour ça que je voulais une
review humaine avant 34at : sur ce genre de chose, l'autosuggestion
à partir d'un listing convaincant-mais-décalé est exactement le mode
de défaillance que je ne peux pas auto-détecter.

Si tu valides l'Option A après désasm propre, je ship.
Si tu valides l'option « retour à la capture putbyte façon Oricutron »,
je rollback la reconstruction RAM (sprints 34aq/34as) sur la partie
TAP layout — en gardant le SDIMG, MBR, fd_kind, mkstemp, $02B1/SEC qui
sont des fixes indépendants.

— Fin de la réponse
