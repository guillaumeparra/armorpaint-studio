# Réglages d'export de textures d'un projet (lot B6), dans son sidecar : la fenêtre d'export (studio_export_window.py)
# les écrit et les remet à l'ouverture ; le publish ShotGrid (lot C4, hooks tk-multi-publish2 du moteur tk-armorpaint)
# rejoue le dernier export avec. Python 3.9+, sans dépendance.
#
# Sidecar du projet : <dossier du .arm>/<nom sans .arm ni version>.export_settings.json ("robot_v003.arm" ->
# "robot.export_settings.json", commun aux versions). ArmorPaint n'y touche pas :
#   "presets" : presets enregistrés par l'artiste ("Save as preset"), au format des presets studio ;
#   "texture_template" : template Toolkit des textures choisi pour ce projet (ShotGrid, lot B6), à la place de celui
#               du réglage texture_templates ; l'op texture_pattern du sidecar ShotGrid le lit ici, pour l'export
#               comme pour le publish ;
#   "export"  : ses choix, remis à l'ouverture de la fenêtre : "preset" ("file:<nom>" pour un preset des dossiers ou
#               livré, "project:<nom>" pour un preset de "presets"), "color_space", "mode" (EXPORT_MODE_*), "padding",
#               "dir", "group", et par preset ("presets" : {clé: {...}}) : "overrides" (bits et résolution par map),
#               "disabled" (maps), "skip" (fichiers décochés, [[texture, variant, tuile], ...]), "naming" (modèle de nom
#               des fichiers).

import json
import os
import re

SIDECAR_SUFFIX = ".export_settings.json"


def sidecar_path(project_path):
    """Sidecar des réglages d'export, à côté du .arm ; None pour un projet jamais enregistré."""
    if not project_path:
        return None
    folder, base = os.path.split(project_path)
    stem = re.sub(r"\.arm$", "", base, flags=re.I)
    stem = re.sub(r"[._-]v\d+$", "", stem, flags=re.I) or stem
    return os.path.join(folder, stem + SIDECAR_SUFFIX)


def read_sidecar(path):
    """(contenu, erreur) ; un sidecar absent est vide, un sidecar illisible aussi (et signalé, jamais écrasé)."""
    if not path or not os.path.exists(path):
        return {}, None
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        if not isinstance(data, dict):
            raise ValueError("not a JSON object")
        return data, None
    except (OSError, ValueError) as e:
        return {}, "%s: %s" % (os.path.basename(path), e)


def texture_template(project_path):
    """Template des textures choisi pour ce projet ("texture_template" du sidecar), None s'il n'y en a pas."""
    data, _ = read_sidecar(sidecar_path(project_path))
    name = data.get("texture_template")
    return name if isinstance(name, str) and name else None


def write_sidecar(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, path)


def preset_settings(value):
    """Réglages d'un preset lus du sidecar ("export" > "presets" > clé), nettoyés : {"overrides", "disabled" (set),
    "skip" (set de triplets), "naming" (None si absent)}."""
    value = value if isinstance(value, dict) else {}
    overrides = value.get("overrides") if isinstance(value.get("overrides"), dict) else {}
    return {
        "overrides": {n: dict(o) for n, o in overrides.items() if isinstance(o, dict)},
        "disabled": {n for n in value.get("disabled") or [] if isinstance(n, str)},
        "skip": {tuple(k) for k in value.get("skip") or [] if isinstance(k, list) and len(k) == 3},
        "naming": value.get("naming") if isinstance(value.get("naming"), str) and value.get("naming") else None,
    }


def skip_list(settings):
    """Fichiers à ne pas écrire (args skip d'une requête export) : les décochés, puis toutes les variantes et tuiles des
    maps désactivées ([map, "*", "*"])."""
    disabled = sorted(settings["disabled"])
    return [list(k) for k in sorted(settings["skip"]) if k[0] not in settings["disabled"]] + [
        [n, "*", "*"] for n in disabled
    ]


def choices_from_sidecar(project_path):
    """Le dernier export de la fenêtre pour ce projet, au format des args de sa requête export (studio_export_window.h),
    repris du sidecar : (choix, erreur). choix None sans sidecar ou sans export enregistré ; erreur : sidecar illisible
    ou preset du projet absent. Les réglages par map sont passés tels quels : ArmorPaint ignore ceux d'une map que le
    preset n'a pas, et une valeur égale à celle du preset ne change rien."""
    path = sidecar_path(project_path)
    data, error = read_sidecar(path)
    prefs = data.get("export")
    if not isinstance(prefs, dict) or not isinstance(prefs.get("preset"), str) or not prefs["preset"]:
        return None, error
    key = prefs["preset"]
    kind, _, name = key.partition(":") if ":" in key else ("file", ":", key)
    per = preset_settings((prefs.get("presets") or {}).get(key) if isinstance(prefs.get("presets"), dict) else None)
    mode = prefs.get("mode")
    args = {
        "preset": name,
        "color_space": prefs.get("color_space") if isinstance(prefs.get("color_space"), str) else "",
        "dir": prefs.get("dir") if isinstance(prefs.get("dir"), str) and prefs.get("dir") else os.path.dirname(project_path),
        "mode": mode if isinstance(mode, int) and not isinstance(mode, bool) else 0,
        "padding": bool(prefs.get("padding", False)),
        "skip": skip_list(per),
        "overrides": per["overrides"],
    }
    if per["naming"]:
        args["naming"] = per["naming"]
    if kind == "project":
        presets = data.get("presets") if isinstance(data.get("presets"), dict) else {}
        if not isinstance(presets.get(name), dict):
            return None, "%s: project preset '%s' is missing" % (os.path.basename(path), name)
        args["preset_json"] = presets[name]
        args["preset_source"] = path
    return args, None
