// Lot A1 : shim C++ au-dessus d'OpenColorIO v2, contrat dans ../studio_ocio_lib.h. Il traduit, il ne décide rien :
// le choix de la configuration, les caches et le découpage en threads appartiennent à studio_ocio.c.

#include "../studio_ocio_lib.h"

#include <OpenColorIO/OpenColorIO.h>

#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <string>

namespace OCIO = OCIO_NAMESPACE;

#define STUDIO_OCIO_EXPORT extern "C" __attribute__((visibility("default")))

#ifdef __APPLE__
// Liaison statique dans ArmorPaint (macOS) : amake ne sait pas ajouter d'option d'édition de liens. Ces options
// voyagent avec l'objet (LC_LINKER_OPTION) : le runtime C++ et les frameworks qu'emploie OCIO (profil ICC de l'écran).
__asm__(".linker_option \"-lc++\"\n"
        ".linker_option \"-framework\", \"CoreFoundation\"\n"
        ".linker_option \"-framework\", \"CoreGraphics\"\n"
        ".linker_option \"-framework\", \"ColorSync\"\n"
        ".linker_option \"-framework\", \"IOKit\"\n");
#endif

struct studio_ocio_lib_config {
	OCIO::ConstConfigRcPtr config;
};

struct studio_ocio_lib_proc {
	OCIO::ConstProcessorRcPtr    processor;
	OCIO::ConstCPUProcessorRcPtr cpu;
};

namespace {

std::string &error_slot() {
	thread_local std::string message;
	return message;
}

void set_error(const char *message) {
	try {
		error_slot() = message;
	}
	catch (...) {
		// Plus de mémoire pour le message : l'appelant voit quand même l'échec par la valeur de retour.
	}
}

// Exécute f() en convertissant toute exception en erreur : rien ne traverse l'ABI C.
template <typename R, typename F> R guarded(R on_error, F f) noexcept {
	error_slot().clear();
	try {
		return f();
	}
	catch (const OCIO::Exception &e) {
		set_error(e.what());
	}
	catch (const std::bad_alloc &) {
		set_error("out of memory");
	}
	catch (const std::exception &e) {
		set_error(e.what());
	}
	catch (...) {
		set_error("unknown exception");
	}
	return on_error;
}

bool check(bool ok, const char *message) {
	if (!ok) {
		set_error(message);
	}
	return ok;
}

bool is_empty(const char *s) {
	return s == nullptr || s[0] == '\0';
}

OCIO::ConstColorSpaceRcPtr find_colorspace(studio_ocio_lib_config_t *cfg, const char *name) {
	if (!check(cfg != nullptr && !is_empty(name), "null config or empty colorspace name")) {
		return nullptr;
	}
	OCIO::ConstColorSpaceRcPtr cs = cfg->config->getColorSpace(name); // accepte aussi rôles et alias
	if (!cs) {
		set_error((std::string("unknown colorspace: ") + name).c_str());
	}
	return cs;
}

studio_ocio_lib_proc_t *make_proc(OCIO::ConstProcessorRcPtr processor) {
	// Processeur CPU par défaut : entrée et sortie float32, optimisation par défaut. C'est ce qu'emploient
	// ocioconvert et PyOpenColorIO, les références de la validation (lot A6).
	OCIO::ConstCPUProcessorRcPtr cpu = processor->getDefaultCPUProcessor();
	return new studio_ocio_lib_proc{processor, cpu};
}

} // namespace

STUDIO_OCIO_EXPORT int studio_ocio_lib_abi(void) {
	return STUDIO_OCIO_LIB_ABI;
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_ocio_version(void) {
	return OCIO::GetVersion();
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_error(void) {
	return error_slot().c_str();
}

STUDIO_OCIO_EXPORT studio_ocio_lib_config_t *studio_ocio_lib_config_load(const char *path_or_uri) {
	return guarded<studio_ocio_lib_config_t *>(nullptr, [&]() -> studio_ocio_lib_config_t * {
		if (!check(!is_empty(path_or_uri), "empty config path")) {
			return nullptr;
		}
		// CreateFromFile accepte aussi les URI ocio:// des configurations intégrées et les archives .ocioz.
		OCIO::ConstConfigRcPtr config = OCIO::Config::CreateFromFile(path_or_uri);
		config->validate();
		return new studio_ocio_lib_config{config};
	});
}

STUDIO_OCIO_EXPORT void studio_ocio_lib_config_free(studio_ocio_lib_config_t *cfg) {
	delete cfg;
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_config_name(studio_ocio_lib_config_t *cfg) {
	return guarded<const char *>(nullptr, [&]() -> const char * {
		return check(cfg != nullptr, "null config") ? cfg->config->getName() : nullptr;
	});
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_config_description(studio_ocio_lib_config_t *cfg) {
	return guarded<const char *>(nullptr, [&]() -> const char * {
		return check(cfg != nullptr, "null config") ? cfg->config->getDescription() : nullptr;
	});
}

STUDIO_OCIO_EXPORT int studio_ocio_lib_count(studio_ocio_lib_config_t *cfg, int list) {
	return guarded<int>(-1, [&]() -> int {
		if (!check(cfg != nullptr, "null config")) {
			return -1;
		}
		const OCIO::Config &c = *cfg->config;
		switch (list) {
		case STUDIO_OCIO_LIB_COLORSPACES:
			return c.getNumColorSpaces(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ACTIVE);
		case STUDIO_OCIO_LIB_DISPLAYS:
			return c.getNumDisplays();
		case STUDIO_OCIO_LIB_LOOKS:
			return c.getNumLooks();
		case STUDIO_OCIO_LIB_ROLES:
			return c.getNumRoles();
		}
		set_error("unknown list");
		return -1;
	});
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_name(studio_ocio_lib_config_t *cfg, int list, int i) {
	return guarded<const char *>(nullptr, [&]() -> const char * {
		int n = studio_ocio_lib_count(cfg, list);
		if (n < 0 || !check(i >= 0 && i < n, "index out of range")) {
			return nullptr;
		}
		const OCIO::Config &c = *cfg->config;
		switch (list) {
		case STUDIO_OCIO_LIB_COLORSPACES:
			return c.getColorSpaceNameByIndex(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ACTIVE, i);
		case STUDIO_OCIO_LIB_DISPLAYS:
			return c.getDisplay(i);
		case STUDIO_OCIO_LIB_LOOKS:
			return c.getLookNameByIndex(i);
		default:
			return c.getRoleName(i);
		}
	});
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_resolve(studio_ocio_lib_config_t *cfg, const char *name) {
	return guarded<const char *>(nullptr, [&]() -> const char * {
		OCIO::ConstColorSpaceRcPtr cs = find_colorspace(cfg, name);
		return cs ? cs->getName() : nullptr;
	});
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_identify_builtin(studio_ocio_lib_config_t *cfg, const char *builtin) {
	return guarded<const char *>(nullptr, [&]() -> const char * {
		if (!check(cfg != nullptr && !is_empty(builtin), "null config or empty colorspace name")) {
			return nullptr;
		}
		OCIO::ConstColorSpaceRcPtr cs = cfg->config->getColorSpace(builtin);
		if (!cs) {
			// Chargée une fois (une exception laisse la statique à initialiser : l'appel suivant réessaie).
			static const OCIO::ConstConfigRcPtr reference = OCIO::Config::CreateFromFile("ocio://default");
			const char *name = OCIO::Config::IdentifyBuiltinColorSpace(cfg->config, reference, builtin);
			cs               = name != nullptr ? cfg->config->getColorSpace(name) : nullptr;
		}
		if (!cs) {
			set_error((std::string("no colorspace matches the built-in ") + builtin).c_str());
			return nullptr;
		}
		return cs->getName();
	});
}

STUDIO_OCIO_EXPORT int studio_ocio_lib_is_data(studio_ocio_lib_config_t *cfg, const char *colorspace) {
	return guarded<int>(-1, [&]() -> int {
		OCIO::ConstColorSpaceRcPtr cs = find_colorspace(cfg, colorspace);
		return cs ? (cs->isData() ? 1 : 0) : -1;
	});
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_encoding(studio_ocio_lib_config_t *cfg, const char *colorspace) {
	return guarded<const char *>(nullptr, [&]() -> const char * {
		OCIO::ConstColorSpaceRcPtr cs = find_colorspace(cfg, colorspace);
		return cs ? cs->getEncoding() : nullptr;
	});
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_family(studio_ocio_lib_config_t *cfg, const char *colorspace) {
	return guarded<const char *>(nullptr, [&]() -> const char * {
		OCIO::ConstColorSpaceRcPtr cs = find_colorspace(cfg, colorspace);
		return cs ? cs->getFamily() : nullptr;
	});
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_colorspace_from_path(studio_ocio_lib_config_t *cfg, const char *path) {
	return guarded<const char *>(nullptr, [&]() -> const char * {
		if (!check(cfg != nullptr && path != nullptr, "null config or path")) {
			return nullptr;
		}
		return cfg->config->getColorSpaceFromFilepath(path);
	});
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_default_display(studio_ocio_lib_config_t *cfg) {
	return guarded<const char *>(nullptr, [&]() -> const char * {
		return check(cfg != nullptr, "null config") ? cfg->config->getDefaultDisplay() : nullptr;
	});
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_default_view(studio_ocio_lib_config_t *cfg, const char *display) {
	return guarded<const char *>(nullptr, [&]() -> const char * {
		if (!check(cfg != nullptr && !is_empty(display), "null config or empty display")) {
			return nullptr;
		}
		return cfg->config->getDefaultView(display);
	});
}

STUDIO_OCIO_EXPORT int studio_ocio_lib_view_count(studio_ocio_lib_config_t *cfg, const char *display) {
	return guarded<int>(-1, [&]() -> int {
		if (!check(cfg != nullptr && !is_empty(display), "null config or empty display")) {
			return -1;
		}
		return cfg->config->getNumViews(display);
	});
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_view_name(studio_ocio_lib_config_t *cfg, const char *display, int i) {
	return guarded<const char *>(nullptr, [&]() -> const char * {
		int n = studio_ocio_lib_view_count(cfg, display);
		if (n < 0 || !check(i >= 0 && i < n, "index out of range")) {
			return nullptr;
		}
		return cfg->config->getView(display, i);
	});
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_view_colorspace(studio_ocio_lib_config_t *cfg, const char *display,
                                                               const char *view) {
	return guarded<const char *>(nullptr, [&]() -> const char * {
		if (!check(cfg != nullptr && !is_empty(display) && !is_empty(view), "null config, empty display or view")) {
			return nullptr;
		}
		const char *cs = cfg->config->getDisplayViewColorSpaceName(display, view);
		if (!check(!is_empty(cs), "unknown display/view")) {
			return nullptr;
		}
		// Vue d'une view_transform partagée : son espace d'affichage porte le nom du display.
		if (strcmp(cs, OCIO::OCIO_VIEW_USE_DISPLAY_NAME) == 0) {
			cs = display;
		}
		OCIO::ConstColorSpaceRcPtr c = cfg->config->getColorSpace(cs);
		return check(c != nullptr, "display colorspace not found") ? c->getName() : nullptr;
	});
}

STUDIO_OCIO_EXPORT studio_ocio_lib_proc_t *studio_ocio_lib_proc_colorspace(studio_ocio_lib_config_t *cfg,
                                                                           const char *src, const char *dst) {
	return guarded<studio_ocio_lib_proc_t *>(nullptr, [&]() -> studio_ocio_lib_proc_t * {
		// Noms vérifiés d'abord : le message d'OCIO sur un nom inconnu ne dit pas lequel des deux manque.
		if (!find_colorspace(cfg, src) || !find_colorspace(cfg, dst)) {
			return nullptr;
		}
		return make_proc(cfg->config->getProcessor(src, dst));
	});
}

STUDIO_OCIO_EXPORT studio_ocio_lib_proc_t *studio_ocio_lib_proc_display(studio_ocio_lib_config_t *cfg,
                                                                        const char *src, const char *display,
                                                                        const char *view, const char *looks,
                                                                        int inverse) {
	return guarded<studio_ocio_lib_proc_t *>(nullptr, [&]() -> studio_ocio_lib_proc_t * {
		if (!find_colorspace(cfg, src) || !check(!is_empty(display) && !is_empty(view), "empty display or view")) {
			return nullptr;
		}
		OCIO::TransformDirection dir = inverse ? OCIO::TRANSFORM_DIR_INVERSE : OCIO::TRANSFORM_DIR_FORWARD;
		if (is_empty(looks)) {
			return make_proc(cfg->config->getProcessor(src, display, view, dir));
		}
		// Look supplémentaire : le pipeline « legacy » d'OCIO l'insère avant la vue, comme le font les applications
		// hôtes. Il n'a pas de sens retour : on inverse le groupe de transformations du processeur aller.
		OCIO::DisplayViewTransformRcPtr dvt = OCIO::DisplayViewTransform::Create();
		dvt->setSrc(src);
		dvt->setDisplay(display);
		dvt->setView(view);
		OCIO::LegacyViewingPipelineRcPtr pipeline = OCIO::LegacyViewingPipeline::Create();
		pipeline->setDisplayViewTransform(dvt);
		pipeline->setLooksOverrideEnabled(true);
		pipeline->setLooksOverride(looks);
		OCIO::ConstProcessorRcPtr forward = pipeline->getProcessor(cfg->config);
		if (!inverse) {
			return make_proc(forward);
		}
		OCIO::GroupTransformRcPtr group = forward->createGroupTransform();
		group->setDirection(OCIO::TRANSFORM_DIR_INVERSE);
		return make_proc(cfg->config->getProcessor(group));
	});
}

STUDIO_OCIO_EXPORT void studio_ocio_lib_proc_free(studio_ocio_lib_proc_t *proc) {
	delete proc;
}

STUDIO_OCIO_EXPORT int studio_ocio_lib_proc_is_noop(studio_ocio_lib_proc_t *proc) {
	return guarded<int>(-1, [&]() -> int {
		return check(proc != nullptr, "null processor") ? (proc->cpu->isNoOp() ? 1 : 0) : -1;
	});
}

STUDIO_OCIO_EXPORT const char *studio_ocio_lib_proc_cache_id(studio_ocio_lib_proc_t *proc) {
	return guarded<const char *>(nullptr, [&]() -> const char * {
		return check(proc != nullptr, "null processor") ? proc->processor->getCacheID() : nullptr;
	});
}

STUDIO_OCIO_EXPORT int studio_ocio_lib_apply(studio_ocio_lib_proc_t *proc, float *pixels, long width, long height,
                                             int channels, long stride) {
	return guarded<int>(0, [&]() -> int {
		if (!check(proc != nullptr && pixels != nullptr, "null processor or pixels") ||
		    !check(channels == 3 || channels == 4, "channels must be 3 or 4") ||
		    !check(width > 0 && height > 0, "empty image")) {
			return 0;
		}
		// Bornes avant tout calcul de taille : une ligne ou une image dont la taille en octets déborde est refusée.
		const long pixel_bytes = (long)sizeof(float) * channels;
		if (!check(width <= INTPTR_MAX / pixel_bytes, "image too wide")) {
			return 0;
		}
		const long row_bytes = width * pixel_bytes;
		if (stride == 0) {
			stride = row_bytes;
		}
		if (!check(stride >= row_bytes, "stride smaller than a row") ||
		    !check(height <= INTPTR_MAX / stride, "image too large")) {
			return 0;
		}
		// Toujours décrite en 3 canaux, avec le pas réel du pixel : en RGBA, l'alpha est hors de l'image et ne peut pas
		// changer. OCIO, lui, le fait passer par la transformation, et certaines vues d'affichage le modifient
		// (ACES 2.0 de la config studio : x1.0000116).
		OCIO::PackedImageDesc img(pixels, width, height, 3, OCIO::BIT_DEPTH_F32, sizeof(float), pixel_bytes, stride);
		proc->cpu->apply(img);
		return 1;
	});
}
