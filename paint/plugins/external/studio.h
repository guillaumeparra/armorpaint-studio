#pragma once

// Code studio compilé dans ArmorPaint via le hook paint/plugins/external/ (define WITH_EXTERNAL).
// Chaque fichier .c d'ici est une unité de compilation séparée de l'app (unity build de paint/sources/) :
// les symboles du cœur s'utilisent par forward declaration, comme dans paint/plugins/io_exr/io_exr.c.

#define STUDIO_VERSION "0.8.1"

// Cœur ArmorPaint / iron
void iron_log(const char *format, ...);
