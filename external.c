#include "studio.h"
#include "studio_exr.h"

// Appelé par plugins_init() (paint/plugins/plugins.c) quand WITH_EXTERNAL est défini, après l'enregistrement des
// importeurs upstream : un module peut donc les remplacer. Chaque module studio s'enregistre ici au fur et à mesure
// des lots (exr, export, ocio, shotgrid).
void external_init(void) {
	iron_log("studio: external %s loaded", STUDIO_VERSION);
	studio_exr_init();
}
