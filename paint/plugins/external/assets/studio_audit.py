# Journal d'audit local (lot C5), côté sidecar ShotGrid : une ligne JSON par évènement, ajoutée au fichier que donne
# ArmorPaint dans $STUDIO_AUDIT_LOG (format et emplacement : external/studio_audit.h). Sans ce fichier, rien n'est
# écrit. Une ligne est écrite d'un seul write() en ajout (O_APPEND) : ArmorPaint écrit dans le même fichier.

import datetime
import getpass
import json
import logging
import os
import socket

log = logging.getLogger("studio_sg")


def path():
    return os.environ.get("STUDIO_AUDIT_LOG") or None


def now():
    """Heure UTC au format du journal : 2026-09-27T10:04:05.123Z."""
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def _user():
    try:
        return getpass.getuser()
    except Exception:  # noqa: BLE001 — pas de compte (conteneur…)
        return ""


def write(event, **fields):
    """Ajoute l'évènement au journal, avec les champs communs (time, source, pid, host, user, event) puis fields
    (les None sont omis). Faux si le journal est désactivé ou l'écriture a échoué (signalé au log, jamais bloquant)."""
    target = path()
    if not target:
        return False
    record = {"time": now(), "source": "sidecar", "pid": os.getpid(), "host": socket.gethostname(), "user": _user(),
              "event": event}
    record.update((k, v) for k, v in fields.items() if v is not None)
    line = json.dumps(record, ensure_ascii=True, separators=(",", ":"), default=str) + "\n"
    try:
        fd = os.open(target, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
        try:
            os.write(fd, line.encode("ascii"))
        finally:
            os.close(fd)
        return True
    except OSError as e:
        log.warning("audit log %s: %s", target, e)
        return False
