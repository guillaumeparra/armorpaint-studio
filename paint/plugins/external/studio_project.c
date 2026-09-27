#include "studio_project.h"
#include "iron_array.h"
#include "iron_gpu.h"
#include "iron_map.h"
#include "iron_obj.h"
#include "iron_string.h"
#include "iron_system.h"
#include "iron_ui.h"
#include "engine.h"
#include "../../sources/types.h"
#include "studio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cœur ArmorPaint (paint/sources/globals.h)
extern project_t *g_project;

#define LISTENERS_MAX 8

static struct {
	void (*on_change[LISTENERS_MAX])(bool opened);
	int on_change_count;
	void (*on_save[LISTENERS_MAX])(string_array_t *texture_files);
	int on_save_count;
	// Projet courant. project_new() garde g_project mais refait ses tableaux ; ouvrir un .arm remplace g_project. Deux
	// signes, comme l'import des textures (studio_ocio_import.c) : l'adresse de g_project ou de son tableau d'assets
	// change, ou le compteur d'assets recule.
	project_t *project;
	void      *assets;
	int        next_asset_id;
	int        serial;
	bool       opened;
} pj;

static void on_update(void *data) {
	(void)data;
	studio_project_check();
}

void studio_project_init(void) {
	sys_notify_on_update(on_update, NULL);
}

void studio_project_on_change(void (*fn)(bool opened)) {
	if (pj.on_change_count < LISTENERS_MAX) {
		pj.on_change[pj.on_change_count++] = fn;
	}
}

void studio_project_on_save(void (*fn)(string_array_t *texture_files)) {
	if (pj.on_save_count < LISTENERS_MAX) {
		pj.on_save[pj.on_save_count++] = fn;
	}
}

void studio_project_check(void) {
	if (g_project == NULL || g_project->_ == NULL) {
		return;
	}
	void *assets     = g_project->_->assets;
	int   next       = g_project->_->next_asset_id;
	bool  replaced   = g_project != pj.project;
	bool  changed    = replaced || assets != pj.assets || next < pj.next_asset_id;
	pj.next_asset_id = next;
	if (!changed) {
		return;
	}
	pj.project = g_project;
	pj.assets  = assets;
	pj.serial++;
	// Un .arm ouvert remplace g_project et a son chemin (import_arm_run_project) ; project_new() garde g_project (un
	// script peut lui donner un chemin avant la détection), le projet du démarrage n'a pas de chemin, sauf un .arm de la
	// ligne de commande (args_parse), ouvert juste après.
	pj.opened = replaced && g_project->_->filepath != NULL && g_project->_->filepath[0] != 0;
	for (int i = 0; i < pj.on_change_count; i++) {
		pj.on_change[i](pj.opened);
	}
}

int studio_project_serial(void) {
	return pj.serial;
}

bool studio_project_opened(void) {
	return pj.opened;
}

static packed_asset_t *find_entry(const char *name, int *index) {
	if (g_project == NULL || g_project->packed_assets == NULL) {
		return NULL;
	}
	for (int i = 0; i < (int)g_project->packed_assets->length; i++) {
		packed_asset_t *pa = g_project->packed_assets->buffer[i];
		if (pa != NULL && pa->name != NULL && strcmp(pa->name, name) == 0) {
			if (index != NULL) {
				*index = i;
			}
			return pa;
		}
	}
	return NULL;
}

char *studio_project_entry(const char *name) {
	packed_asset_t *pa    = find_entry(name, NULL);
	buffer_t       *bytes = pa != NULL ? (buffer_t *)pa->bytes : NULL; // struct buffer : buffer_t (types.h)
	if (bytes == NULL) {
		return NULL;
	}
	char *text = malloc((size_t)bytes->length + 1);
	if (text != NULL) {
		memcpy(text, bytes->buffer, bytes->length);
		text[bytes->length] = 0;
	}
	return text;
}

bool studio_project_set_entry(const char *name, const char *text) {
	if (g_project == NULL) {
		return false;
	}
	int             index = -1;
	packed_asset_t *pa    = find_entry(name, &index);
	if (text == NULL) {
		if (pa != NULL) {
			array_splice((any_array_t *)g_project->packed_assets, index, 1);
		}
		return true;
	}
	size_t    len   = strlen(text);
	buffer_t *bytes = buffer_create((uint32_t)len);
	if (bytes == NULL) {
		return false;
	}
	memcpy(bytes->buffer, text, len);
	if (pa != NULL) {
		pa->bytes = (struct buffer *)bytes; // l'ancienne peut venir du décodage du projet (bloc unique) : pas libérée
		return true;
	}
	if (g_project->packed_assets == NULL) {
		g_project->packed_assets = (packed_asset_t_array_t *)any_array_create_from_raw((void *[]){}, 0); // comme project_new
	}
	pa = malloc(sizeof(packed_asset_t));
	if (pa == NULL) {
		return false;
	}
	pa->name  = strdup(name);
	pa->bytes = (struct buffer *)bytes;
	any_array_push((any_array_t *)g_project->packed_assets, pa);
	return true;
}

void studio_project_save_entries(string_array_t *texture_files) {
	studio_project_check();
	for (int i = 0; i < pj.on_save_count; i++) {
		pj.on_save[i](texture_files);
	}
}
