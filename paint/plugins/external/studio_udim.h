#pragma once

// UDIM avant les UDIM natifs (chantier D, ARMORPAINT_UDIM.md §3) : les tuiles se partagent chaque texture de calque, en
// grille de stride × stride cases. Chaque tuile n'a que res / stride pixels de côté, ré-agrandis à l'export par tuile.

// Conseil donné après un import UDIM : tiles tuiles peintes (au moins 2) en grille stride × stride, projet res_x × res_y,
// calques de bits 8, 16 ou 32 par canal. Dit les pixels de chaque tuile et, si c'est moins que 2048 ou 4096, la
// résolution de projet qu'il faut pour les avoir, avec la mémoire d'un calque et les pas d'annulation qu'elle laisse
// (layers_resize : 4 à 8192 px, 1 au-delà). NULL si tiles < 2 ou valeurs hors bornes. malloc.
char *studio_udim_hint(int tiles, int stride, int res_x, int res_y, int bits);
