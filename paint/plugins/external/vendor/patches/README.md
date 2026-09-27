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

## deflate-hdist-19

| | |
|---|---|
| Fichier | `exr_deflate.c`, `ht_build()` et ses trois appels dans `decode_dynamic_tables()` |
| Trouvé | 2026-09-27, par `.tests/ocio_import_app.sh` (EXR ZIP écrits par OpenEXR 3.5, via libdeflate) |
| Version tinyexr | `644148d0` (2026-09-07) |

**Défaut.** `ht_build()` déduisait le type de table de son nombre de codes : 19 (`DFL_CODELEN_CODES`) → table des
longueurs de code, plus de 32 (`DFL_DIST_CODES`) → littéraux/longueurs, sinon distances. Un bloc dynamique peut annoncer
HDIST = 19 codes de distance : sa table de distances prend alors le type 0. Ses entrées rapides
(`dfl_fast_entry()`) n'incluent plus le nombre de bits supplémentaires, alors que `decode_block_fast()` les
retranche en consommant le code (`consume - extra`). Le lecteur de bits se désynchronise : erreur « corrupt »,
ou pire, sortie fausse sans erreur. Sur 3000 flux zlib aléatoires, 402 ont HDIST = 19 au premier bloc. 238 d'entre
eux échouent : 221 en erreur, 17 en sortie fausse sans erreur, dont 3 de la bonne taille (le contrôle de taille
d'`exr_zip.c` ne les voit pas). Aucun échec avec un autre HDIST. Une texture EXR ZIP ordinaire exportée par
OpenEXR 3.5 ne s'importait pas.

**Correctif.** `ht_build()` reçoit le type en paramètre ; les appelants passent 0 (longueurs de code),
1 (littéraux/longueurs) et 2 (distances).

**Test.** `.tests/run.sh`, passe `check` : `hdist19_stream` (346 octets, HDIST = 19) doit donner les 700 octets
attendus. Sans correctif : 134 octets, sans erreur. Balayage ponctuel, 3000 flux zlib aléatoires comparés à zlib :
0 écart après correctif.

**Signes que l'amont l'a corrigé.** `ht_build()` reçoit le type explicitement, ou ne le déduit plus de `count`.
Dans ce cas, supprimer ce patch.

## zlib-adler32

| | |
|---|---|
| Fichier | `exr_deflate.c`, `exr_inflate_zlib()` |
| Trouvé | 2026-09-27, en analysant `deflate-hdist-19` |
| Version tinyexr | `644148d0` (2026-09-07) |

**Défaut.** `exr_inflate_zlib()` sautait la somme Adler-32 de fin de flux sans la vérifier. Un défaut du décodeur
(voir `deflate-hdist-19`) ou un fichier abîmé qui produit la taille attendue passait donc pour des pixels valides.
zlib, libdeflate (le backend `exr_ld_inflate_zlib()` de tinyexr) et OpenEXR la vérifient.

**Correctif.** Comparer `exr_adler32()` de la sortie aux 4 derniers octets du flux, sinon `EXR_ERROR_CORRUPT`.
Comme OpenEXR 3, qui exige que le flux occupe tout le bloc, on lit la somme en fin de bloc. Coût : une passe
d'`exr_adler32()` sur les octets décodés.

**Test.** `.tests/run.sh`, passe `check` : `hdist19_stream` avec son dernier octet modifié doit être refusé.
Balayage ponctuel : les 3000 flux à somme modifiée sont refusés, et avec ce seul patch (sans `deflate-hdist-19`),
les 238 flux mal décodés deviennent tous des erreurs.

**Signes que l'amont l'a corrigé.** Un appel à `exr_adler32()` (ou une lecture de la somme) dans
`exr_inflate_zlib()`. Dans ce cas, supprimer ce patch.
