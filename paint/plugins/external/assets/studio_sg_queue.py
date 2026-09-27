# File d'attente des publishes (lot C5). Quand ShotGrid devient injoignable pendant un publish, les fichiers sont déjà
# écrits (copie du projet, export des textures) : leur enregistrement dans ShotGrid (sgtk.util.register_publish) est
# gardé ici, puis rejoué quand le site répond de nouveau (op replay_publishes, studio_sg_session.py).
#
# Dossier $STUDIO_SG_QUEUE (celui des préférences d'ArmorPaint, voir external/studio_sg_offline.h) : un fichier JSON
# par publish, <clé>.json, et sa vignette, <clé>.<ext> (celle du publish est un fichier temporaire). La clé commence
# par l'heure de la mise en file : l'ordre des noms est celui des publishes, un parent (le projet) avant ses enfants
# (ses textures). Écriture par un fichier temporaire renommé. Un publish écarté part dans discarded/.
#
# Un enregistrement :
#   version, key, created (heure UTC de la mise en file), site, project_id, task_id, context (Context.to_dict())
#   path, name, version_number, published_file_type, comment, created_at (celle demandée au publish, sinon None)
#   dependency_ids : PublishedFile dont il dépend ; depends_on : clés de ses parents encore en file
#   kwargs : les autres arguments de register_publish (dependency_paths, sg_fields, created_by…), en JSON
#   thumbnail : nom de sa vignette dans le dossier, None sans vignette
#   entity_id : son PublishedFile, s'il a été créé avant l'échec (vignette ou dépendances pas envoyées)
#   error, attempts, last_attempt : le dernier échec (mise en file, puis rejeux)

import datetime
import itertools
import json
import logging
import os
import re
import shutil

log = logging.getLogger("studio_sg")

VERSION = 1
DISCARDED = "discarded"
_serial = itertools.count(1)
_KEY = re.compile(r"[0-9A-Za-z][0-9A-Za-z_-]*")
_THUMBNAIL = re.compile(r"[0-9A-Za-z][0-9A-Za-z_.-]*")


def valid_key(key):
    """Une clé de add() : ni chemin ni extension (les clés viennent aussi d'ArmorPaint, op discard_publishes)."""
    return isinstance(key, str) and _KEY.fullmatch(key) is not None


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc)


def iso(moment):
    return moment.strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def parse_iso(text):
    """Heure d'iso() (ou d'isoformat()) en datetime UTC, None si illisible."""
    if not text:
        return None
    try:
        moment = datetime.datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return None
    return moment if moment.tzinfo is not None else moment.replace(tzinfo=datetime.timezone.utc)


class Queue(object):
    def __init__(self, directory):
        self.directory = directory

    @classmethod
    def from_env(cls):
        directory = os.environ.get("STUDIO_SG_QUEUE")
        return cls(os.path.abspath(directory)) if directory else None

    def _path(self, key):
        return os.path.join(self.directory, key + ".json")

    def _write(self, record):
        os.makedirs(self.directory, exist_ok=True)
        path = self._path(record["key"])
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(record, f, indent=1, sort_keys=True)
            f.write("\n")
        os.replace(tmp, path)

    def keys(self):
        try:
            names = os.listdir(self.directory)
        except OSError:
            return []
        return sorted(n[:-5] for n in names if n.endswith(".json"))

    def count(self):
        return len(self.keys())

    def load(self, key):
        if not valid_key(key):
            return None
        try:
            with open(self._path(key), encoding="utf-8") as f:
                record = json.load(f)
        except (OSError, ValueError) as e:
            log.warning("publish queue: %s unreadable: %s", self._path(key), e)
            return None
        return record if isinstance(record, dict) and record.get("key") == key else None

    def records(self):
        """Les publishes en file, dans l'ordre de leur mise en file ; un fichier illisible est sauté (et signalé)."""
        return [r for r in (self.load(k) for k in self.keys()) if r is not None]

    def add(self, record, thumbnail=None):
        """Met le publish en file (record sans clé), avec une copie de sa vignette. Rend la clé. Lève une exception si
        le dossier ne peut pas être écrit, ou si un champ n'est pas du JSON."""
        now = utc_now()
        key = "%s-%d-%d" % (now.strftime("%Y%m%dT%H%M%S%fZ"), os.getpid(), next(_serial))
        record = dict(record, version=VERSION, key=key, created=iso(now), attempts=0, last_attempt=None, thumbnail=None)
        json.dumps(record)  # refuse avant d'écrire quoi que ce soit
        os.makedirs(self.directory, exist_ok=True)
        if thumbnail and os.path.isfile(thumbnail):
            name = key + (os.path.splitext(thumbnail)[1] or ".png")
            shutil.copyfile(thumbnail, os.path.join(self.directory, name))
            record["thumbnail"] = name
        self._write(record)
        return key

    def save(self, record):
        self._write(record)

    def _thumbnail(self, record):
        """La vignette dans le dossier, None sans vignette. Un nom de fichier seul : un enregistrement modifié à la main
        (ou déposé dans une file partagée) ne fait ni lire, ni supprimer, ni déplacer un fichier hors de la file."""
        name = record.get("thumbnail")
        if not name:
            return None
        if not isinstance(name, str) or _THUMBNAIL.fullmatch(name) is None:
            log.warning("publish queue: %s: thumbnail %r ignored (not a file name)", record.get("key"), name)
            return None
        return os.path.join(self.directory, name)

    def thumbnail_path(self, record):
        path = self._thumbnail(record)
        return path if path and os.path.isfile(path) else None

    def _files(self, record):
        files = [self._path(record["key"])]
        thumbnail = self._thumbnail(record)
        if thumbnail:
            files.append(thumbnail)
        return files

    def remove(self, record):
        for path in self._files(record):
            try:
                os.remove(path)
            except FileNotFoundError:
                pass

    def discard(self, record):
        """Écarte le publish : ses fichiers vont dans discarded/, il ne sera jamais rejoué."""
        target = os.path.join(self.directory, DISCARDED)
        os.makedirs(target, exist_ok=True)
        for path in self._files(record):
            if os.path.exists(path):
                os.replace(path, os.path.join(target, os.path.basename(path)))
