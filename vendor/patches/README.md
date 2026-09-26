# Correctifs locaux de tinyexr

`vendor/.tinyexr/` est une copie de tinyexr v3, régénérée par `tools/update_tinyexr.sh`. Les fichiers de ce
dossier sont les correctifs qu'on lui applique, réappliqués automatiquement à la fin de chaque mise à jour.
Un patch qui ne s'applique plus **arrête** la mise à jour.

Chaque correctif :
- porte un marqueur `STUDIO-VENDOR-PATCH(<id>)` dans le code, avec la raison ;
- a un `<id>.patch` ici, en chemins `a/<fichier>` / `b/<fichier>` relatifs à `vendor/.tinyexr/` ;
- est décrit ci-dessous, avec le cas de test qui le motive.

C'est le pendant de `PATCHES.md` (qui, lui, couvre les fichiers upstream d'ArmorPaint), avec la même règle :
rien n'est proposé en PR, on corrige chez nous et on réanalyse à chaque mise à jour.

Pour refaire un patch après une mise à jour qui le casse : éditer `vendor/.tinyexr/<fichier>`, puis
`diff -u <checkout tinyexr>/src/<fichier> vendor/.tinyexr/<fichier>` en corrigeant les deux lignes d'en-tête
en `--- a/<fichier>` et `+++ b/<fichier>`. Relancer ensuite `.tests/run.sh`.

---

## jph-d64-null

| | |
|---|---|
| Fichier | `exr_jph.c`, `jph_decode_codeblock()` |
| Trouvé | 2026-09-26, par `.tests/run.sh` (fuzz sur `rgba_htj2k32.exr`, graine 1) |
| Version tinyexr | `644148d0` (2026-09-07) |

**Défaut.** Les plans de composantes sont alloués **soit** en `int32_t` (`d32`) **soit** en `int64_t`
(`d64`), jamais les deux (`jph_alloc_component_planes()`). `jph_decode_codeblock()` prend la voie i32 quand
`d32` existe *et* que `missing_msbs < 30` *et* que `band->kmax <= 30` ; sinon elle prend la voie i64. Un
fichier HTJ2K dont un code-block annonce `missing_msbs >= 30` ou `kmax > 30` alors que les plans ont été
alloués en i32 tombe donc dans la voie i64 avec `d64 == NULL` : l'adresse de destination est calculée à
partir de NULL, puis écrite. Le garde d'entrée de la fonction ne teste que `!d32 && !d64`, ce qui ne couvre
pas ce cas.

**Correctif.** `if (!plane->d64) return EXR_ERROR_CORRUPT;` avant le calcul de `dst`. Un fichier qui demande
des coefficients au-delà de 30 bits sur des plans 32 bits est incohérent : c'est bien un fichier corrompu.

**Test.** `.tests/run.sh` — le fuzz HTJ2K plantait en une passe de 2000 mutations (`SEGV` dans
`jph_decode_block64_cleanup`, adresse 0x80). Après correctif : refusé proprement
(`corrupt or malformed data`), et les fichiers HTJ2K valides se décodent toujours (`rgba_htj2k256.exr`,
`rgba_htj2k32.exr` dans la passe `check`).

**Signes que l'amont l'a corrigé.** Un test de `d64` (ou une allocation des deux plans) dans
`jph_decode_codeblock()`. Dans ce cas, supprimer ce patch.
