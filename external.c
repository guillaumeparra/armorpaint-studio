#include "studio.h"

// Appelé par plugins_init() (paint/plugins/plugins.c) quand WITH_EXTERNAL est défini.
// Chaque module studio s'enregistre ici au fur et à mesure des lots (exr, export, ocio, shotgrid).
void external_init(void) {
	iron_log("studio: external %s loaded", STUDIO_VERSION);
}
