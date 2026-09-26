#include "studio.h"
#include "studio_atlas.h"
#include "studio_exec.h"
#include "studio_export.h"
#include "studio_export_window.h"
#include "studio_export_naming.h"
#include "studio_exr.h"
#include "studio_ocio.h"
#include "studio_ocio_view.h"
#include "studio_shotgrid.h"
#include "studio_workfiles.h"

// Appelé par plugins_init() (paint/plugins/plugins.c) quand WITH_EXTERNAL est défini, après l'enregistrement des
// importeurs upstream : un module peut donc les remplacer. Chaque module studio s'enregistre ici au fur et à mesure
// des lots (exr, export, ocio, shotgrid).
void external_init(void) {
	iron_log("studio: external %s loaded", STUDIO_VERSION);
	studio_exr_init();
	studio_ocio_init();
	studio_ocio_view_init();
	studio_exec_init();
	studio_shotgrid_init();
	studio_workfiles_init();
	studio_export_naming_init();
	studio_atlas_init();
	studio_export_init();
	studio_export_window_init();
}
