# Journal d'audit local (lot C5), côté sidecar ShotGrid : une ligne JSON par évènement, ajoutée au fichier que donne
# ArmorPaint dans $STUDIO_AUDIT_LOG (format et emplacement : external/studio_audit.h). Sans ce fichier, rien n'est
# écrit. Une ligne est écrite d'un seul write() en ajout (O_APPEND) : ArmorPaint écrit dans le même fichier.
#
# Envoi au site (send) : les lignes pas encore envoyées deviennent des EventLogEntry du site de la session, par l'op
# send_audit, ou check_online avec "audit" (la sonde d'ArmorPaint). Où l'envoi en est : <journal>.sent,
# {"head": empreinte de la première ligne du fichier, "offset": octets traités} ; l'empreinte reconnaît le fichier
# après une rotation (.1, faite par ArmorPaint au démarrage), dont la fin part avant le nouveau journal. Deux sidecars
# (deux ArmorPaint) n'envoient pas en même temps : verrou sur <journal>.sent.lock.

import datetime
import fnmatch
import getpass
import hashlib
import json
import logging
import os
import re
import socket
import time

try:
    import fcntl
except ImportError:  # Windows : pas de sidecar (lot C2)
    fcntl = None

log = logging.getLogger("studio_sg")

EVENT_TYPE = "ArmorPaint_%s"  # event_type des EventLogEntry : ArmorPaint_project_save, ArmorPaint_publish…
SEND_CHUNK = 50  # lignes par requête (batch)
SEND_BUDGET_S = 2.0  # le sidecar sert une requête à la fois : l'envoi rend la main, la suite part à l'appel suivant
_EVENT = re.compile(r"^[A-Za-z0-9_]{1,64}$")


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


# ---- Envoi au site ----------------------------------------------------------------------------------------------------


def send_patterns():
    """Évènements envoyés au site, d'après STUDIO_AUDIT_SEND : motifs séparés par des virgules (publish*,sg_*), tous
    par défaut ; None pour aucun (none)."""
    value = (os.environ.get("STUDIO_AUDIT_SEND") or "*").strip()
    if value.lower() == "none":
        return None
    return [p.strip() for p in value.split(",") if p.strip()] or ["*"]


def _site_key(url):
    return (url or "").strip().rstrip("/").lower()


def _positive(value):
    return value if isinstance(value, int) and not isinstance(value, bool) and value > 0 else None


def entry(record):
    """L'EventLogEntry d'une ligne du journal : event_type ArmorPaint_<évènement>, la ligne entière dans meta (son
    heure : meta.time, created_at étant celle de l'envoi), le projet (project_id, ou celui du contexte sg) et, pour un
    publish enregistré, le PublishedFile."""
    event = record["event"]
    context = record.get("sg") if isinstance(record.get("sg"), dict) else {}
    detail = next((str(record[k]) for k in ("path", "reason", "key") if record.get(k)), "")
    data = {
        "event_type": EVENT_TYPE % event,
        "description": "ArmorPaint %s%s (%s@%s, %s)" % (event, ": " + detail if detail else "", record.get("user") or "?",
                                                        record.get("host") or "?", record.get("time") or "?"),
        "meta": record,
    }
    project = _positive(record.get("project_id")) or _positive(context.get("project_id"))
    if project:
        data["project"] = {"type": "Project", "id": project}
    published = _positive(record.get("id")) if event in ("publish", "publish_replayed") else None
    if published:
        data["entity"] = {"type": "PublishedFile", "id": published}
    return data


def _head(filename):
    """Empreinte de la première ligne du fichier, qui le reconnaît après une rotation ; None s'il n'en a pas."""
    try:
        with open(filename, "rb") as f:
            line = f.readline()
    except OSError:
        return None
    return hashlib.sha1(line).hexdigest() if line.endswith(b"\n") else None


def _lines(filename, offset):
    """Lignes complètes du fichier après offset (tout le fichier s'il est plus court) : [(octet de fin, ligne)]. La
    dernière, sans fin de ligne, n'est pas finie d'écrire : elle attend."""
    try:
        with open(filename, "rb") as f:
            size = f.seek(0, os.SEEK_END)
            offset = offset if 0 <= offset <= size else 0
            f.seek(offset)
            data = f.read(size - offset)
    except OSError as e:
        log.warning("audit log %s: %s", filename, e)
        return []
    lines = []
    for raw in data.split(b"\n")[:-1]:
        offset += len(raw) + 1
        lines.append((offset, raw))
    return lines


class _Refused(Exception):
    """Le site refuse une entrée, même sans ses liens (droits, session expirée…) : l'envoi s'arrête là."""


class _Sender(object):
    def __init__(self, sg, site, unreachable, patterns, cursor_path, chunk, budget_s):
        self.sg = sg
        self.site = _site_key(site)
        self.unreachable = unreachable
        self.patterns = patterns
        self.cursor_path = cursor_path
        self.chunk = chunk
        self.deadline = time.monotonic() + budget_s
        self.head = None
        self.result = {"sent": 0, "skipped": 0, "more": False, "error": None}

    def cursor(self):
        try:
            with open(self.cursor_path) as f:
                data = json.load(f)
            return data.get("head"), int(data.get("offset") or 0)
        except (OSError, ValueError, TypeError, AttributeError):
            return None, 0

    def save(self, offset):
        tmp = "%s.%d.tmp" % (self.cursor_path, os.getpid())
        with open(tmp, "w") as f:
            json.dump({"head": self.head, "offset": offset}, f)
        os.replace(tmp, self.cursor_path)

    def prepare(self, raw):
        """L'entrée d'une ligne, None si elle ne part pas : illisible, évènement filtré, autre site."""
        try:
            record = json.loads(raw.decode("utf-8"))
        except ValueError:
            return None
        if not isinstance(record, dict) or not isinstance(record.get("event"), str) or not _EVENT.match(record["event"]):
            return None
        if not any(fnmatch.fnmatchcase(record["event"], p) for p in self.patterns):
            return None
        context = record.get("sg") if isinstance(record.get("sg"), dict) else {}
        site = record.get("site") or context.get("site")
        if site and isinstance(site, str) and _site_key(site) != self.site:
            return None  # envoyée par une session sur ce site-là
        return entry(record)

    def run(self, target):
        head, offset = self.cursor()
        current = _head(target)
        if head is not None and head == current:
            sources = [(target, current, offset)]
        elif head is not None and _head(target + ".1") == head:
            sources = [(target + ".1", head, offset), (target, current, 0)]  # la fin du journal d'avant la rotation
        else:
            sources = [(target, current, 0)]  # premier envoi, ou journal remplacé : depuis le début
        for filename, self.head, start in sources:
            batch = []
            for end, raw in _lines(filename, start):
                batch.append((end, self.prepare(raw)))
                if len(batch) >= self.chunk:
                    if not self.flush(batch):
                        return
                    batch = []
                    if time.monotonic() > self.deadline:
                        self.result["more"] = True
                        return
            if batch and not self.flush(batch):
                return
            if filename != target:
                self.head = current
                self.save(0)

    def flush(self, batch):
        """Crée les entrées du lot en une requête (batch : tout ou rien), le curseur après. Refusé par le site : une
        par une. Faux si l'envoi s'arrête (_Refused)."""
        entries = [data for _, data in batch if data is not None]
        if entries:
            try:
                self.sg.batch([{"request_type": "create", "entity_type": "EventLogEntry", "data": d} for d in entries])
            except Exception as e:
                if self.unreachable(e):
                    raise
                log.warning("audit: batch refused by ShotGrid (%s), entries sent one by one", e)
                return self.one_by_one(batch)
        self.result["sent"] += len(entries)
        self.result["skipped"] += len(batch) - len(entries)
        self.save(batch[-1][0])
        return True

    def one_by_one(self, batch):
        for end, data in batch:
            if data is None:
                self.result["skipped"] += 1
            else:
                try:
                    self.create(data)
                except _Refused as e:
                    self.result["error"] = str(e)
                    log.warning("audit: not sent to ShotGrid: %s", e)
                    return False
                self.result["sent"] += 1
            self.save(end)
        return True

    def create(self, data):
        """Une entrée ; refusée, elle repart sans ses liens (projet ou publish effacé depuis) avant d'abandonner."""
        try:
            self.sg.create("EventLogEntry", data)
            return
        except Exception as e:
            if self.unreachable(e):
                raise
            if "project" not in data and "entity" not in data:
                raise _Refused("%s: %s" % (type(e).__name__, e))
            error = e
        try:
            self.sg.create("EventLogEntry", {k: v for k, v in data.items() if k not in ("project", "entity")})
        except Exception as e:
            if self.unreachable(e):
                raise
            raise _Refused("%s: %s" % (type(e).__name__, e))
        log.warning("audit: %s sent without its links (%s)", data["event_type"], error)


def send(sg, site, unreachable, chunk=SEND_CHUNK, budget_s=SEND_BUDGET_S):
    """Envoie au site les lignes du journal pas encore envoyées, dans l'ordre, par lots, en EventLogEntry (entry).
    Ne partent pas (skipped) : les lignes illisibles, les évènements hors de STUDIO_AUDIT_SEND et ceux d'un autre site.
    Rend {"sent", "skipped", "more", "error"} : more, il en reste (budget de temps épuisé : la suite au prochain
    appel) ; error, le refus du site qui arrête l'envoi (droits, session expirée…), les lignes suivantes attendent ;
    "disabled" sans journal ou avec STUDIO_AUDIT_SEND=none, "busy" si un autre sidecar envoie. ShotGrid injoignable :
    l'exception remonte, ce qui est parti est noté. Une réponse perdue (lot créé, curseur pas encore écrit) le fait
    renvoyer : au moins une fois, pas exactement une fois. unreachable(e) : l'erreur dit-elle le site injoignable."""
    target = path()
    patterns = send_patterns()
    if not target or patterns is None:
        return {"sent": 0, "skipped": 0, "more": False, "error": None, "disabled": True}
    sender = _Sender(sg, site, unreachable, patterns, target + ".sent", max(1, chunk), budget_s)
    if fcntl is None or not os.path.exists(target):
        return sender.result
    lock = os.open(target + ".sent.lock", os.O_RDWR | os.O_CREAT, 0o644)
    try:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            sender.result["busy"] = True
            return sender.result
        sender.run(target)
    finally:
        os.close(lock)  # rend le verrou
    return sender.result
