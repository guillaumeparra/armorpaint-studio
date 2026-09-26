# Environnement d'un contexte, comme tk-config-default2 : project, asset, asset_step, shot, shot_step.
from tank import Hook


class PickEnvironment(Hook):
    def execute(self, context, **kwargs):
        if context.project is None:
            return None
        if context.entity is None:
            return "project"
        kind = {"Asset": "asset", "Shot": "shot"}.get(context.entity["type"])
        if kind is None:
            return None
        return kind + "_step" if context.step else kind
