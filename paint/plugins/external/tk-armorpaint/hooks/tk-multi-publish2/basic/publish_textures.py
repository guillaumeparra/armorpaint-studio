# Publish des textures d'un projet ArmorPaint (lot C4), éléments armorpaint.texture du collecteur. Configuration :
#   hook: "{self}/publish_file.py:{engine}/tk-multi-publish2/basic/publish_textures.py"
#   settings: {"Publish Template": <template des textures publiées, facultatif>, "Publish Type": Texture,
#              "UDIM Publish Type": UDIM Image}
#
# Au publish de la première texture, ArmorPaint exporte (requête host publish_export) avec les réglages et les noms de
# la collecte, sans les textures décochées. Chaque texture est ensuite enregistrée dans ShotGrid, une séquence <UDIM>
# en un seul publish, dépendante du publish du projet s'il est publié dans le même passage. Le template des textures
# du collecteur sert de template de travail : avec un "Publish Template", les fichiers y sont copiés ; sans, ils sont
# publiés en place. Un nom que ce template refuse (canal base_color face à une clé filter_by: alphanumeric, extension
# hors des choices) bloque le publish avec un "Publish Template", n'est qu'un avertissement sans. Le template des
# textures doit contenir {version} : un publish ne remplace jamais un autre. Une texture sans version dans son chemin
# est décochée (l'export suivant réécrirait le fichier publié).

import os

import sgtk

HookBaseClass = sgtk.get_hook_baseclass()


class ArmorPaintTexturesPublishPlugin(HookBaseClass):
    @property
    def icon(self):
        return os.path.join(self.parent.disk_location, "hooks", "icons", "publish.png")

    @property
    def description(self):
        return """
        Exports the texture with the settings of the last texture export of the project, then publishes it to Flow
        Production Tracking. A UDIM texture is one publish, its path holding <code>&lt;UDIM&gt;</code> in place of
        the tile. The textures are exported once for all the checked textures; unchecked ones are not written.
        """

    @property
    def settings(self):
        settings = dict(super().settings or {})
        settings.update(
            {
                "Publish Template": {
                    "type": "template",
                    "default": None,
                    "description": "Template of the published textures (templates.yml). Without it, the textures "
                    "are published where the export writes them.",
                },
                "Publish Type": {
                    "type": "shotgun_publish_type",
                    "default": "Texture",
                    "description": "Publish type of a texture.",
                },
                "UDIM Publish Type": {
                    "type": "shotgun_publish_type",
                    "default": "UDIM Image",
                    "description": "Publish type of a UDIM texture (one publish for all its tiles).",
                },
            }
        )
        return settings

    @property
    def item_filters(self):
        return ["armorpaint.texture"]

    def accept(self, settings, item):
        if not item.properties.get("versioned"):
            # Publiée en place sans version, la texture serait réécrite par l'export suivant : décochée.
            self.logger.warning(
                "%s has no version in its name: the next texture export would overwrite the published file. Name "
                "the textures by a ShotGrid template with {version}, or with <file> or <version>." % (item.name,)
            )
            return {"accepted": True, "checked": False}
        return {"accepted": True, "checked": True}

    def validate(self, settings, item):
        mismatch = item.parent.properties.get("context_mismatch") if item.parent is not None else None
        if mismatch:  # collector.py : publiées dans ce contexte, elles iraient à la tâche d'un autre élément
            raise Exception(mismatch)
        shared = self._shared(item)
        shared["export"] = None  # la validation précède chaque publish : un nouvel export
        if shared.get("naming_error"):
            raise Exception("Texture names from the ShotGrid template: %s" % (shared["naming_error"],))
        blocking = [i.get("message") for i in shared["report"].get("issues") or [] if i.get("level") == "blocking"]
        if blocking:
            raise Exception("The texture export is blocked: %s" % ("; ".join(blocking),))
        kind = "UDIM Publish Type" if item.properties.get("udims") else "Publish Type"
        item.properties["publish_type"] = settings.get(kind).value
        name = settings.get("Publish Template").value
        publish_template = self.parent.engine.get_template_by_name(name) if name else None
        if name and publish_template is None:
            raise Exception("Publish template '%s' is not defined in the project configuration." % (name,))
        if publish_template is not None:
            if item.properties.get("work_template") is None:
                raise Exception(
                    "Publish template '%s' needs the textures named by a ShotGrid template with {version}." % (name,)
                )
            item.properties["publish_template"] = publish_template
        self._check_work_template(item, publish_template is not None)
        if item.properties.get("udims"):
            self._check_udim_key(item)
        return super().validate(settings, item)

    def publish(self, settings, item):
        shared = self._shared(item)
        if shared.get("export") is None:
            shared["export"] = self._export(item.parent, shared)
        export = shared["export"]
        if not export.get("ok"):
            raise Exception("Texture export failed: %s" % (export.get("error") or "see the ArmorPaint console",))
        files = self._exported_files(item, export.get("report") or {})
        missing = [f for f in files if not os.path.exists(f)]
        if missing:
            raise Exception("Texture files not written: %s" % (", ".join(missing),))
        super().publish(settings, item)

    def finalize(self, settings, item):
        finalize = super().finalize  # ShotGrid injoignable : voir le moteur (lot C5)
        self.parent.engine.finalize_publish(item, self.logger, lambda: finalize(settings, item))

    def _check_work_template(self, item, copied):
        # Les noms viennent du template des textures, mais le canal et l'extension y sont mis par ArmorPaint à la place
        # des marqueurs <channel> et <ext>, que les filtres des clés n'ont pas vus (op texture_pattern) : un canal
        # base_color face à filter_by: alphanumeric, une extension hors des choices de sa clé font un chemin que le
        # template refuse. tk-multi-publish2 publie alors en place sans erreur, et tk-core ne dit pas quelle clé
        # refuse : elle est cherchée ici.
        template = item.properties.get("work_template")
        path = item.properties["path"]
        if template is None or template.validate(path):
            return
        session = self.parent.engine.sidecar_session()
        keys = session.settings["texture_keys"] if session is not None else {}
        variant, texture = item.properties["variant"], item.properties["texture"]
        values = {"channel": "%s_%s" % (variant, texture) if variant else texture, "ext": os.path.splitext(path)[1][1:]}
        refused = []
        for role in ("channel", "ext"):
            key = template.keys.get(keys.get(role))
            if key is not None and not key.validate(values[role]):
                refused.append(getattr(key, "_last_error", "") or "the key %s refuses %r" % (key.name, values[role]))
        message = "%s does not match the texture template %s (templates.yml): %s." % (
            os.path.basename(path),
            template.name,
            "; ".join(refused) or "a key of the template refuses a part of the name",
        )
        if copied:
            # Publiée en place, la texture resterait dans le dossier de travail, hors du Publish Template.
            raise Exception(
                "%s Fix the key in templates.yml (channel names like base_color need a key without filter_by: "
                "alphanumeric, the extension must be in the choices of its key), then collect again." % (message,)
            )
        self.logger.warning(
            "%s The tools that read the path with the template (Loader, Breakdown) will not recognize it." % (message,)
        )

    def _check_udim_key(self, item):
        # ShotGrid enregistre une séquence sous la valeur par défaut de sa clé (register_publish) : sans
        # default: "<UDIM>" dans templates.yml, %04d, que les chargeurs de textures UDIM (Maya : "UDIM Image") ne
        # reconnaissent pas.
        template = item.properties.get("publish_template") or item.properties.get("work_template")
        for key in template.keys.values() if template is not None else ():
            if isinstance(key, sgtk.SequenceKey) and key.default not in ("<UDIM>", "$UDIM"):
                name = os.path.basename(item.properties["path"]).replace("<UDIM>", key.default)
                self.logger.warning(
                    "The key %s of the template %s has the default %s, not <UDIM>: Flow Production Tracking will list "
                    'the texture as %s. Set default: "<UDIM>" on the key in templates.yml.'
                    % (key.name, template.name, key.default, name)
                )

    # ---- Export ------------------------------------------------------------------------------------------------------

    @staticmethod
    def _shared(item):
        shared = item.parent.properties.get("armorpaint_textures") if item.parent is not None else None
        if not shared:
            raise Exception("Texture collected without its project: collect again.")
        return shared

    def _export(self, session_item, shared):
        """Export de toutes les textures cochées, une fois : les fichiers des textures décochées sont écartés."""
        skip = []
        for child in session_item.children:
            if child.type_spec == "armorpaint.texture" and (
                not child.active or not any(task.active for task in child.tasks)
            ):
                skip.extend(child.properties.get("files") or [])
        args = {"naming": shared["naming"], "skip": skip}
        if shared.get("choices") is not None:
            args["choices"] = shared["choices"]
        self.logger.info("Exporting the textures (%s)..." % (shared.get("source"),))
        reply = self.parent.engine.host_request("publish_export", args)
        if reply.get("path") != shared["path"]:
            raise Exception("The open project is now %s: collect again." % (reply.get("path"),))
        export = reply.get("export") or {}
        for issue in (export.get("report") or {}).get("issues") or []:
            if issue.get("level") != "note":
                self.logger.warning("Texture export: %s" % (issue.get("message"),))
        if export.get("ok"):
            self.logger.info("Textures exported to %s" % ((export.get("report") or {}).get("destination"),))
        return export

    def _exported_files(self, item, report):
        """Les fichiers écrits pour cette texture, d'après le rapport de l'export (les tuiles ont pu changer depuis la
        collecte) ; mêmes noms qu'à la collecte, sinon erreur."""
        key = (item.properties["texture"], item.properties["variant"])
        files = [
            f
            for f in report.get("files") or []
            if not f.get("skip") and ((f.get("texture") or ""), (f.get("variant") or "")) == key
        ]
        if not files:
            raise Exception("%s is no longer written by the export: collect again." % (item.name,))
        path = files[0].get("sequence") if item.properties.get("udims") else files[0]["path"]
        if path != item.properties["path"]:
            raise Exception("The export now writes %s instead of %s: collect again." % (path, item.properties["path"]))
        if item.properties.get("udims"):
            item.properties["sequence_paths"] = [f["path"] for f in files]
            item.properties["udims"] = [f["udim"] for f in files if f.get("udim")]
        return [f["path"] for f in files]
