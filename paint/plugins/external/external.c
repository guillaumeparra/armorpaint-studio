#include "studio.h"
#include "studio_atlas.h"
#include "studio_audit.h"
#include "studio_env.h"
#include "studio_batch.h"
#include "studio_exec.h"
#include "studio_export.h"
#include "studio_export_window.h"
#include "studio_export_naming.h"
#include "studio_exr.h"
#include "studio_ocio.h"
#include "studio_ocio_import.h"
#include "studio_ocio_prefs.h"
#include "studio_ocio_view.h"
#include "studio_prefs.h"
#include "studio_project.h"
#include "studio_sg_context.h"
#include "studio_sg_offline.h"
#include "studio_shotgrid.h"
#include "studio_workfiles.h"
#include "subzero.h"

// Appelé par plugins_init() (paint/plugins/plugins.c) quand WITH_EXTERNAL est défini, après l'enregistrement des
// importeurs upstream : un module peut donc les remplacer. Chaque module studio s'enregistre ici au fur et à mesure
// des lots (exr, export, ocio, shotgrid).
void external_init(void) {
	iron_log("studio: external %s loaded", STUDIO_VERSION);
	studio_env_init(); // en premier : studio.env donne des variables lues par les modules suivants (lot E5)
	studio_prefs_init();
	studio_project_init();
	studio_audit_init(); // avant le sidecar ShotGrid, qui écrit dans le même journal
	studio_exr_init();
	studio_ocio_prefs_init(); // préférences couleur prises avant le chargement de la configuration
	studio_ocio_init();
	studio_ocio_view_init();
	studio_ocio_import_init();
	studio_exec_init();
	studio_shotgrid_init();
	studio_sg_context_init();
	studio_sg_offline_init();
	studio_workfiles_init();
	studio_export_naming_init();
	studio_atlas_init();
	studio_export_init();
	studio_export_window_init();
	subzero_init();
	studio_batch_init(); // en dernier : vérifie la ligne de commande avec les presets et OCIO chargés (lot B7)
}
