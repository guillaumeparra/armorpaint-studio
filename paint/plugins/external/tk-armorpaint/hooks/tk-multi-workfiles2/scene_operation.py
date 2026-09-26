# Opérations de tk-multi-workfiles2 sur la scène d'ArmorPaint (hook_scene_operation:
# "{engine}/tk-multi-workfiles2/scene_operation.py"). Chacune est une requête host au processus ArmorPaint (voir
# engine.py), qui l'exécute à sa frame suivante et répond quand c'est fait (fichier écrit ou chargé).

import sgtk

HookClass = sgtk.get_hook_baseclass()


class SceneOperation(HookClass):
    def execute(self, operation, file_path, context, parent_action, file_version, read_only, **kwargs):
        """
        :param operation: current_path, open, save, save_as, reset, prepare_new
        :param file_path: fichier à ouvrir ou chemin de save_as
        :param context: contexte de l'opération (déjà courant pour open et save_as)
        :param parent_action: open_file, new_file, save_file_as, version_up
        :returns: current_path : le chemin du projet ouvert ("" s'il n'a jamais été sauvé) ; reset : True si la scène
                  peut être remplacée ; sinon None
        """
        engine = self.parent.engine
        info = engine.context_info(context)
        if operation == "current_path":
            return engine.host_request("current_path").get("path") or ""
        if operation == "open":
            engine.host_request("open", {"path": file_path, "context": info})
        elif operation == "save":
            engine.host_request("save", {"context": info})
        elif operation == "save_as":
            engine.host_request("save_as", {"path": file_path, "context": info})
        elif operation == "reset":
            # Avant open, rien à faire : l'ouverture remplace le projet. Avant new_file : nouveau projet.
            if parent_action == "new_file":
                engine.host_request("new", {"context": info})
            return True
        elif operation == "prepare_new":
            pass  # le nouveau projet est prêt depuis reset
        return None
