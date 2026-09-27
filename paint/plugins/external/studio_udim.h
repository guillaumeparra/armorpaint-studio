#pragma once

// UDIM natifs (chantier D, lot D2, ARMORPAINT_UDIM.md) : chaque tuile a son jeu de textures de calque, à la résolution
// du projet ; la mémoire d'un calque est multipliée par le nombre de tuiles.

// Conseil donné après un import UDIM : tiles tuiles peintes (2 à 64, celles que les calques tiennent), projet
// res_x × res_y, calques de bits 8, 16 ou 32 par canal. Dit la résolution des textures de chaque tuile et la mémoire
// d'un calque. NULL si tiles hors de 2 à 64 ou valeurs hors bornes. malloc.
char *studio_udim_hint(int tiles, int res_x, int res_y, int bits);
