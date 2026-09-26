#include "../../../../base/tools/amake/amake.h"

// Réglages de build du code studio. amake évalue ce fichier par l'injection external-project de
// paint/plugins/project.c (voir PATCHES.md) : external/ ne peut pas, sinon, ajouter de bibliothèque ni de define. Le
// dossier commence par un point : amake ne compile pas ce fichier dans l'app.
// fs_exists résout un chemin relatif contre ce dossier ; add_lib ne résout rien, d'où le chemin absolu.

project_t *main() {
	project_t *project = project_create("studio");

	// Lot A1 : sur macOS, OCIO est lié en statique (.ocio/lib/libstudio_ocio.a, produit par tools/build_ocio.sh).
	// Chargé par dlopen, il serait refusé : ArmorPaint est signé ad hoc avec le hardened runtime, dont la validation
	// des bibliothèques n'accepte que du code de la même équipe. Sans l'archive, l'app se construit sans OCIO.
	if (platform == PLATFORM_MACOS && fs_exists("../.ocio/lib/libstudio_ocio.a")) {
		add_lib(project, string("%s/plugins/external/.ocio/lib/libstudio_ocio.a", flags->dirname));
		add_define(project, "WITH_STUDIO_OCIO_STATIC");
	}

	return project;
}
