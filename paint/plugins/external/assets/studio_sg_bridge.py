#!/usr/bin/env python3
# Sidecar ShotGrid d'ArmorPaint (lot C2). Lancé et supervisé par external/studio_shotgrid.c ; les opérations sont
# dans studio_sg_session.py. Python 3.9+, aucune dépendance hors tk-core (qui embarque shotgun_api3).
#
# Protocole, une ligne JSON par message (ASCII, les caractères non ASCII sont échappés) :
#   au démarrage   <- {"id":0,"ok":true,"data":{"protocol":1,...}}
#   requête        -> {"id":7,"op":"list_tasks","args":{...}}
#   réponse        <- {"id":7,"ok":true,"data":...}
#                  <- {"id":7,"ok":false,"error":{"code":"auth_required","message":"..."}}
#   requête host   <- {"host":1,"op":"open","args":{...}}         pendant une requête, le sidecar demande à ArmorPaint
#   réponse host   -> {"host":1,"ok":true,"data":...}             (ou "ok":false,"error") ; voir HostChannel
# "id" est toujours le premier champ : le superviseur apparie requête et réponse sans parser le JSON.
# Une réponse portant "restart":true annonce que le sidecar s'arrête juste après : le superviseur le relance et
# rejoue la requête (changement de projet Toolkit, voir studio_sg_session.py).
# Une réponse porte "sg" quand la requête a parlé au site ShotGrid : {"online":true}, ou {"online":false,"reason":...}
# s'il était injoignable (lot C5, mode dégradé : le superviseur en tient l'état hors ligne d'ArmorPaint). C'est le
# résultat du dernier appel réseau de la requête (voir network()), ou d'une erreur sg_unreachable.
#
# stdout est réservé au protocole. Tout le reste — logs, print() d'un hook, sortie d'un sous-processus — part sur
# stderr, que le superviseur recopie dans le log d'ArmorPaint. stdin est réservé aussi : rien d'autre ne peut le
# lire, en particulier une invite de mot de passe. Le sidecar s'arrête sur EOF de stdin (ArmorPaint fermé ou tué).

import datetime
import json
import logging
import os
import re
import select
import sys
import traceback

PROTOCOL = 1

log = logging.getLogger("studio_sg")

HOST = None  # canal vers ArmorPaint (HostChannel), posé par main() ; utilisé par le moteur tk-armorpaint
SESSION = None  # la session (studio_sg_session.Session), posée par main() ; pour les hooks du moteur (publish)

# Joignabilité du site ShotGrid (lot C5). OFFLINE : raison du dernier appel réseau en échec, None depuis le dernier
# appel réussi (d'une requête à l'autre). _seen : ce que la requête en cours en a vu, pour le champ "sg" de sa réponse.
OFFLINE = None
_seen = None


def network(online, reason=None):
    """Résultat d'un appel au site ShotGrid : abouti (online), ou injoignable (reason : le message de l'erreur).
    Appelé à chaque appel de l'API ShotGrid (Session._watch_network) et quand un publish est mis en file d'attente."""
    global OFFLINE, _seen
    OFFLINE = None if online else (reason or "ShotGrid unreachable")
    _seen = {"online": True} if online else {"online": False, "reason": OFFLINE}


class BridgeError(Exception):
    """Erreur rapportée au superviseur : un code stable (voir README.md) et un message lisible par l'artiste."""

    def __init__(self, code, message, restart=False, details=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.restart = restart
        self.details = details  # membres de plus dans l'objet "error" de la réponse (dict), None sinon


class HostError(Exception):
    """Refus ou échec d'ArmorPaint sur une requête host (code et message de sa réponse)."""

    def __init__(self, code, message):
        super().__init__("ArmorPaint: %s (%s)" % (message, code))
        self.code = code


# Boucle d'événements de l'interface du sidecar, posée par le moteur tk-armorpaint quand il crée son QApplication. Le
# sidecar l'appelle toutes les IDLE_MS quand il attend ArmorPaint entre deux requêtes : un processus qui a ouvert une
# fenêtre est une application pour macOS, qui la déclare bloquée (roue, clavier et souris retenus quand elle a le
# premier plan) si elle ne lit plus ses événements.
IDLE = None
IDLE_MS = 50


class LineReader(object):
    """readline() sur le descripteur du protocole. Sans tampon caché devant le descripteur : select() dit vrai, et
    l'attente peut faire tourner idle() (hors Windows, où select() ne prend pas les pipes)."""

    def __init__(self, fd):
        self.fd = fd
        self.buf = b""

    def readline(self, idle=None):
        while b"\n" not in self.buf:
            if idle is not None and os.name != "nt" and not select.select([self.fd], [], [], IDLE_MS / 1000.0)[0]:
                try:
                    idle()
                except Exception:  # noqa: BLE001
                    log.exception("idle callback failed, disabled")
                    idle = None
                continue
            chunk = os.read(self.fd, 65536)
            if not chunk:
                line, self.buf = self.buf, b""
                return line.decode("utf-8", "replace")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return (line + b"\n").decode("utf-8", "replace")


class HostChannel(object):
    """Requêtes du sidecar vers ArmorPaint, pendant une requête d'ArmorPaint (op show_app : les opérations sur la scène
    que demande une app Toolkit). Le superviseur les passe au thread de l'interface d'ArmorPaint, qui répond à la frame
    suivante ; la requête en cours d'ArmorPaint attend pendant ce temps, rien d'autre ne peut arriver sur stdin."""

    def __init__(self, proto_in, send):
        self._in = proto_in
        self._send = send
        self._next = 0

    def request(self, op, args=None):
        self._next += 1
        number = self._next
        message = {"host": number, "op": op, "args": args or {}}
        self._send(json.dumps(message, default=_json_default, ensure_ascii=True, allow_nan=False, separators=(",", ":")))
        while True:
            line = self._in.readline()
            if not line:
                raise HostError("host_gone", "ArmorPaint closed the connection")
            line = line.strip()
            try:
                reply = json.loads(line) if line else None
            except ValueError:
                reply = None
            if not isinstance(reply, dict) or reply.get("host") != number:
                if line:
                    log.warning("ignored while waiting for ArmorPaint: %.200s", line)
                continue
            if reply.get("ok"):
                return reply.get("data")
            error = reply.get("error") or {}
            raise HostError(error.get("code", "host_error"), error.get("message", "no message"))


def _claim_stdio():
    # Garde des copies privées de stdin/stdout pour le protocole, puis branche les descripteurs 0 et 1 du processus
    # sur /dev/null et stderr : un sous-processus lancé par un hook les hérite sans pouvoir corrompre le protocole.
    # os.dup() crée des descripteurs non héritables.
    proto_in = LineReader(os.dup(0))
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


def encode(message_id, ok, payload, restart=False, sg=None):
    # Ordre des clés fixe, "id" en tête (voir l'en-tête). NaN et infinis ne sont pas du JSON : refusés.
    message = {"id": message_id, "ok": ok}
    if ok:
        message["data"] = payload
    else:
        message["error"] = payload
    if restart:
        message["restart"] = True
    if sg is not None:
        message["sg"] = sg
    return json.dumps(message, default=_json_default, ensure_ascii=True, allow_nan=False, separators=(",", ":"))


_ID_PREFIX = re.compile(r'^\s*\{\s*"id"\s*:\s*(\d+)')
_HOST_PREFIX = re.compile(r'^\s*\{\s*"host"\s*:')


def _sg(error=None):
    # Le champ "sg" de la réponse : ce que la requête a vu du réseau ; une erreur sg_unreachable le dit aussi (sans
    # appel observé : un site simulé, une erreur de connexion avant l'appel).
    if error is not None and error.code == "sg_unreachable" and (_seen is None or _seen["online"]):
        network(False, error.message)
    return _seen


def handle_line(session, line):
    """Traite une ligne de requête, retourne (ligne de réponse, arrêter après)."""
    global _seen
    _seen = None
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
        return encode(message_id, True, data, sg=_sg()), request["op"] == "shutdown"
    except BridgeError as e:
        payload = dict(e.details or {}, code=e.code, message=e.message)
        return encode(message_id, False, payload, e.restart, _sg(e)), e.restart
    except Exception as e:  # noqa: BLE001 — tout doit devenir une réponse, sinon le superviseur attend jusqu'au timeout
        error = session.classify(e)
        if error.code == "internal":
            log.error("%s failed:\n%s", line[:200], traceback.format_exc())
        payload = {"code": error.code, "message": error.message}
        return encode(message_id, False, payload, error.restart, _sg(error)), error.restart


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
    global HOST, SESSION
    HOST = HostChannel(proto_in, send)
    SESSION = session

    while True:
        line = proto_in.readline(idle=IDLE)
        if not line:
            break  # EOF : ArmorPaint est parti
        line = line.strip()
        if not line:
            continue
        if _HOST_PREFIX.match(line):
            log.warning("late reply from ArmorPaint ignored: %.200s", line)  # requête host déjà abandonnée
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
