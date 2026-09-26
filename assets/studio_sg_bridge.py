#!/usr/bin/env python3
# Sidecar ShotGrid d'ArmorPaint (lot C2). Lancé et supervisé par external/studio_shotgrid.c ; les opérations sont
# dans studio_sg_session.py. Python 3.9+, aucune dépendance hors tk-core (qui embarque shotgun_api3).
#
# Protocole, une ligne JSON par message (ASCII, les caractères non ASCII sont échappés) :
#   au démarrage   <- {"id":0,"ok":true,"data":{"protocol":1,...}}
#   requête        -> {"id":7,"op":"list_tasks","args":{...}}
#   réponse        <- {"id":7,"ok":true,"data":...}
#                  <- {"id":7,"ok":false,"error":{"code":"auth_required","message":"..."}}
# "id" est toujours le premier champ : le superviseur apparie requête et réponse sans parser le JSON.
# Une réponse portant "restart":true annonce que le sidecar s'arrête juste après : le superviseur le relance et
# rejoue la requête (changement de projet Toolkit, voir studio_sg_session.py).
#
# stdout est réservé au protocole. Tout le reste — logs, print() d'un hook, sortie d'un sous-processus — part sur
# stderr, que le superviseur recopie dans le log d'ArmorPaint. stdin est réservé aussi : rien d'autre ne peut le
# lire, en particulier une invite de mot de passe. Le sidecar s'arrête sur EOF de stdin (ArmorPaint fermé ou tué).

import datetime
import json
import logging
import os
import re
import sys
import traceback

PROTOCOL = 1

log = logging.getLogger("studio_sg")


class BridgeError(Exception):
    """Erreur rapportée au superviseur : un code stable (voir README.md) et un message lisible par l'artiste."""

    def __init__(self, code, message, restart=False):
        super().__init__(message)
        self.code = code
        self.message = message
        self.restart = restart


def _claim_stdio():
    # Garde des copies privées de stdin/stdout pour le protocole, puis branche les descripteurs 0 et 1 du processus
    # sur /dev/null et stderr : un sous-processus lancé par un hook les hérite sans pouvoir corrompre le protocole.
    # os.dup() crée des descripteurs non héritables.
    proto_in = os.fdopen(os.dup(0), "r", encoding="utf-8", errors="replace", newline="\n")
    proto_out = os.fdopen(os.dup(1), "w", encoding="ascii", newline="\n")
    null = os.open(os.devnull, os.O_RDONLY)
    os.dup2(null, 0)
    os.close(null)
    os.dup2(2, 1)
    sys.stdin = open(os.devnull, "r")
    sys.stdout = sys.stderr
    return proto_in, proto_out


def _json_default(value):
    if isinstance(value, (datetime.datetime, datetime.date, datetime.time)):
        return value.isoformat()
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    if isinstance(value, (set, tuple)):
        return list(value)
    return str(value)


def encode(message_id, ok, payload, restart=False):
    # Ordre des clés fixe, "id" en tête (voir l'en-tête). NaN et infinis ne sont pas du JSON : refusés.
    message = {"id": message_id, "ok": ok}
    if ok:
        message["data"] = payload
    else:
        message["error"] = payload
    if restart:
        message["restart"] = True
    return json.dumps(message, default=_json_default, ensure_ascii=True, allow_nan=False, separators=(",", ":"))


_ID_PREFIX = re.compile(r'^\s*\{\s*"id"\s*:\s*(\d+)')


def handle_line(session, line):
    """Traite une ligne de requête, retourne (ligne de réponse, arrêter après)."""
    message_id = None
    match = _ID_PREFIX.match(line)
    if match:
        message_id = int(match.group(1))
    try:
        try:
            request = json.loads(line)
        except ValueError as e:
            raise BridgeError("bad_request", "invalid JSON request: %s" % e)
        if not isinstance(request, dict) or not isinstance(request.get("op"), str):
            raise BridgeError("bad_request", 'a request is {"id":<int>,"op":<string>,"args":<object>}')
        message_id = request.get("id", message_id)
        args = request.get("args")
        if args is None:
            args = {}
        if not isinstance(args, dict):
            raise BridgeError("bad_request", "args must be a JSON object")
        data = session.call(request["op"], args)
        return encode(message_id, True, data), request["op"] == "shutdown"
    except BridgeError as e:
        return encode(message_id, False, {"code": e.code, "message": e.message}, e.restart), e.restart
    except Exception as e:  # noqa: BLE001 — tout doit devenir une réponse, sinon le superviseur attend jusqu'au timeout
        error = session.classify(e)
        if error.code == "internal":
            log.error("%s failed:\n%s", line[:200], traceback.format_exc())
        return encode(message_id, False, {"code": error.code, "message": error.message}, error.restart), error.restart


def main(session_factory=None):
    proto_in, proto_out = _claim_stdio()
    logging.basicConfig(
        stream=sys.stderr,
        level=logging.DEBUG if os.environ.get("STUDIO_SG_DEBUG") else logging.INFO,
        format="%(levelname)s %(name)s: %(message)s",
    )

    def send(text):
        proto_out.write(text + "\n")
        proto_out.flush()

    try:
        if session_factory is None:
            from studio_sg_session import Session

            session_factory = Session
        session = session_factory()
        hello = session.hello()
    except Exception as e:  # noqa: BLE001
        log.error("startup failed:\n%s", traceback.format_exc())
        send(encode(0, False, {"code": "internal", "message": "sidecar startup failed: %s" % e}))
        return 1
    hello["protocol"] = PROTOCOL
    send(encode(0, True, hello))

    while True:
        line = proto_in.readline()
        if not line:
            break  # EOF : ArmorPaint est parti
        line = line.strip()
        if not line:
            continue
        response, stop = handle_line(session, line)
        send(response)
        if stop:
            break
    session.close()
    return 0


if __name__ == "__main__":
    # Lancé comme script, ce fichier est le module __main__ ; studio_sg_session importe studio_sg_bridge, qui en
    # serait une seconde copie avec une autre classe BridgeError. On passe donc par le module importé.
    import studio_sg_bridge

    sys.exit(studio_sg_bridge.main())
