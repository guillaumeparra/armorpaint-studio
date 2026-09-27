# Opérations de la recette C6 (tools/recette_c6.py), ajoutées au sidecar par STUDIO_SG_HOOK. Le hook du studio
# (STUDIO_RECIPE_STUDIO_HOOK, le STUDIO_SG_HOOK d'origine) est chargé d'abord : la recette voit le sidecar du studio.
# Chaque opération rend {"rows": [[état, point, détail, à faire], ...]}, repris tels quels dans le rapport.

import fnmatch
import importlib.util
import os

OK, WARN, FAIL, INFO = "ok", "attention", "ÉCHEC", "info"


def _studio_hook(session):
    path = os.environ.get("STUDIO_RECIPE_STUDIO_HOOK")
    if not path:
        return
    spec = importlib.util.spec_from_file_location("studio_sg_studio_hook", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if hasattr(module, "setup"):
        module.setup(session)


def setup(session):
    _studio_hook(session)
    session.ops["recipe_site"] = lambda args: recipe_site(session, args)
    session.ops["recipe_config"] = lambda args: recipe_config(session, args)


def recipe_site(session, args):
    """Configurations de pipeline du projet (project_id) : celle que prendra l'amorçage d'ArmorPaint (plugin id
    basic.armorpaint par défaut) doit exister, sinon STUDIO_SG_BASE_CONFIGURATION."""
    sg = session._connect()
    project = {"type": "Project", "id": int(args["project_id"])}
    fields = ["code", "descriptor", "uploaded_config", "plugin_ids", "sg_plugin_ids", "mac_path", "linux_path",
              "windows_path", "users"]
    schema = sg.schema_field_read("PipelineConfiguration")
    configs = sg.find("PipelineConfiguration", [["project", "is", project]], [f for f in fields if f in schema])
    plugin_id = session.settings["plugin_id"]
    rows = []
    if not configs:
        if session.settings["base_configuration"]:
            rows.append([OK, "configuration de pipeline", "aucune dans le projet : repli sur %s" %
                         session.settings["base_configuration"], ""])
        else:
            rows.append([FAIL, "configuration de pipeline", "aucune dans le projet",
                         "une PipelineConfiguration pour le projet, ou STUDIO_SG_BASE_CONFIGURATION"])
        return {"rows": rows}
    matching = 0
    for c in configs:
        ids = c.get("plugin_ids") or c.get("sg_plugin_ids") or ""
        patterns = [p.strip() for p in ids.split(",") if p.strip()]
        match = any(fnmatch.fnmatch(plugin_id, p) for p in patterns)
        matching += match
        where = c.get("descriptor") or (c.get("uploaded_config") and "config envoyée sur le site") or c.get(
            "mac_path") or c.get("linux_path") or c.get("windows_path") or "?"
        users = ", ".join(u.get("name", "") for u in c.get("users") or [])
        rows.append([INFO, "configuration %s" % c.get("code"), "%s ; plugin_ids %s%s%s" % (
            where, ids or "(aucun)", " (retenue pour %s)" % plugin_id if match else "",
            " ; bac à sable de %s" % users if users else ""), ""])
    if not matching and not any(c.get("mac_path") or c.get("linux_path") or c.get("windows_path") for c in configs):
        rows.append([WARN, "configuration pour %s" % plugin_id, "aucune n'a de plugin_ids qui le couvre",
                     "plugin_ids « basic.* » sur la configuration du projet, ou STUDIO_SG_PLUGIN_ID"])
    return {"rows": rows}


def _template_row(tk, name, what, keys=(), udim=None):
    if not name:
        return [WARN, what, "pas de template réglé", ""]
    template = tk.templates.get(name)
    if template is None:
        return [FAIL, what, "template %s absent de la configuration" % name, "l'ajouter à core/templates.yml (README)"]
    missing = [k for k in keys if k not in template.keys]
    if missing:
        return [FAIL, what, "%s : clé(s) %s absente(s) (%s)" % (name, ", ".join(missing), template.definition),
                "clés attendues : %s" % ", ".join(keys)]
    if udim and udim in template.keys and template.keys[udim].default != "<UDIM>":
        return [WARN, what, "%s : clé %s sans default: \"<UDIM>\"" % (name, udim),
                "sans lui, ShotGrid enregistre les séquences en %04d, que les chargeurs UDIM ne reconnaissent pas"]
    return [OK, what, "%s : %s" % (name, template.definition), ""]


def recipe_config(session, args):
    """Le moteur tk-armorpaint dans le contexte de la tâche (task_id) : apps, templates, réglages de tk-multi-publish2,
    types de publish du site."""
    engine = session._engine_for(args)  # amorçage, dossiers de la tâche ; app_unavailable sans le moteur
    tk = engine.sgtk
    rows = []
    env = getattr(engine, "environment", None) or {}
    rows.append([OK, "moteur %s %s" % (engine.name, engine.version), "environnement %s, contexte %s" % (
        env.get("name", "?"), engine.context), ""])
    config = getattr(tk, "configuration_descriptor", None)
    rows.append([INFO, "configuration amorcée", config.get_uri() if config is not None else tk.pipeline_configuration
                 .get_path(), ""])

    apps = {a.name: a for a in engine.apps.values()}
    for name in ("tk-multi-workfiles2", "tk-multi-publish2"):
        app = apps.get(name)
        if app is None:
            rows.append([FAIL, "app %s" % name, "absente de l'environnement %s" % env.get("name", "?"),
                         "l'ajouter à engines.tk-armorpaint.apps (README, C3 et C4)"])
        else:
            rows.append([OK, "app %s %s" % (name, app.version), "", ""])

    entity_type = (engine.context.entity or {}).get("type")
    keys = session.settings["texture_keys"]
    rows.append(_template_row(tk, session.settings["work_templates"].get(entity_type),
                              "template de travail (%s)" % entity_type, ("name", "version")))
    rows.append(_template_row(tk, session.settings["texture_templates"].get(entity_type),
                              "template des textures (%s)" % entity_type, (keys["channel"], keys["ext"]), keys["udim"]))

    types = [session.settings["publish_type"]]
    publish = apps.get("tk-multi-publish2")
    if publish is not None:
        collector = publish.get_setting("collector") or ""
        if "tk-multi-publish2/basic/collector.py" in collector and "{engine}" in collector:
            rows.append([OK, "collecteur du publish", collector, ""])
        else:
            rows.append([FAIL, "collecteur du publish", collector or "(aucun)",
                         "collector: '{self}/collector.py:{engine}/tk-multi-publish2/basic/collector.py'"])
        preset = (publish.get_setting("collector_settings") or {}).get("Export Preset")
        rows.append([INFO, "preset d'export du publish", preset or
                     "aucun : réglages de la fenêtre d'export du projet, sinon l'export courant", ""])
        plugins = publish.get_setting("publish_plugins") or []
        for hook, what, template_keys in (("publish_session", "plugin du projet", ()),
                                          ("publish_textures", "plugin des textures", (keys["channel"],))):
            plugin = next((p for p in plugins if hook in (p.get("hook") or "")), None)
            if plugin is None:
                rows.append([FAIL, what, "pas de plugin avec le hook %s.py du moteur" % hook,
                             "publish_plugins de tk-multi-publish2 (README, C4)"])
                continue
            settings = plugin.get("settings") or {}
            template = settings.get("Publish Template")
            if template:
                row = _template_row(tk, template, "%s « %s », Publish Template" % (what, plugin.get("name")),
                                    template_keys, keys["udim"] if hook == "publish_textures" else None)
            else:
                row = [WARN, "%s « %s »" % (what, plugin.get("name")), "sans Publish Template : publié en place",
                       "un template de publish (README, C4)"]
            rows.append(row)
            if hook == "publish_textures":
                types += [settings.get("Publish Type") or "Texture", settings.get("UDIM Publish Type") or "UDIM Image"]

    sg = session._connect()
    found = {t["code"] for t in sg.find("PublishedFileType", [["code", "in", types]], ["code"])}
    for code in dict.fromkeys(types):
        if code in found:
            rows.append([OK, "type de publish « %s »" % code, "existe sur le site", ""])
        else:
            rows.append([INFO, "type de publish « %s »" % code, "absent : créé au premier publish",
                         "droit de créer des PublishedFileType pour le compte de l'artiste, sinon le créer avant"])
    return {"rows": rows}
