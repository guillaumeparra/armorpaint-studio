#!/usr/bin/env python3
# Navigateur de paquets de subzero autonome (lot E9) : panneau à gauche de la fenêtre, comme l'étagère de Painter ou
# le navigateur d'ArmorPaint. Un onglet par dossier : ceux de $SUBZERO_LIBRARY (séparés par ':', ';' sous Windows),
# puis ceux ajoutés par « + », gardés dans <config>/browser.json et retirables (la croix de l'onglet ne supprime rien
# sur le disque). Un onglet liste les .sbsar et .sbs de son dossier et de ses sous-dossiers (dossiers cachés et liens
# symboliques ignorés), avec l'icône du premier graphe, gardée dans le cache (par chemin, date et taille). Double clic
# ou Entrée : ouvrir ; glisser vers la fenêtre ou un autre logiciel ; clic droit : ouvrir, montrer dans le dossier,
# copier le chemin.
#
# Configuration : $SUBZERO_CONFIG_DIR, sinon ~/Library/Application Support/subzero (macOS), %APPDATA%\subzero
# (Windows), $XDG_CONFIG_HOME/subzero (~/.config/subzero). browser.json : {"folders": [dossiers ajoutés], "tab":
# dossier de l'onglet courant, "visible": panneau ouvert}.

import base64
import collections
import hashlib
import json
import os
import re
import subprocess
import sys
import threading

from PySide6 import QtCore, QtGui, QtWidgets

import subzero_meta
import subzero_render
from studio_qt import ERROR, MUTED, log, section_title

EXTENSIONS = (".sbsar", ".sbs")
ICON = 80  # côté des icônes (pixels logiques)
GRID = QtCore.QSize(ICON + 16, ICON + 34)
WIDTH = 316  # trois colonnes
MAX_ITEMS = 5000  # paquets listés par onglet
SBS_ICON_BYTES = 64 << 20  # lecture d'un .sbs pour son icône
CONFIG = "browser.json"
PATH_ROLE = QtCore.Qt.UserRole
# Icône d'un graphe dans un .sbs : <icon><datalength v="n"/><strdata v="PNG en base64"/></icon>
SBS_ICON = re.compile(rb'<icon>\s*<datalength v="\d+"\s*/>\s*<strdata v="([A-Za-z0-9+/=]+)"')


def config_dir():
    if os.environ.get("SUBZERO_CONFIG_DIR"):
        return os.path.expanduser(os.environ["SUBZERO_CONFIG_DIR"])
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/subzero")
    if sys.platform == "win32":
        return os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "subzero")
    return os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "subzero")


def load_config():
    try:
        with open(os.path.join(config_dir(), CONFIG), encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def save_config(data):
    folder = config_dir()
    path = os.path.join(folder, CONFIG)
    try:
        os.makedirs(folder, exist_ok=True)
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            json.dump(data, f, indent=1)
        os.replace(path + ".tmp", path)
    except OSError as e:
        log("could not write %s: %s" % (path, e))


def library_folders():
    """Dossiers de $SUBZERO_LIBRARY, dans l'ordre, sans doublon."""
    out = []
    for part in (os.environ.get("SUBZERO_LIBRARY") or "").split(os.pathsep):
        part = part.strip()
        if part:
            path = os.path.abspath(os.path.expanduser(part))
            if path not in out:
                out.append(path)
    return out


def scan(folder):
    """Paquets du dossier et de ses sous-dossiers, triés par nom ; lève OSError si le dossier n'existe pas."""
    if not os.path.isdir(folder):
        raise OSError("folder not found: %s" % folder)
    found = []
    for root, dirs, names in os.walk(folder, onerror=None, followlinks=False):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for n in names:
            if n.lower().endswith(EXTENSIONS) and not n.startswith("."):
                found.append(os.path.join(root, n))
                if len(found) >= MAX_ITEMS:
                    break
        if len(found) >= MAX_ITEMS:
            break
    return sorted(found, key=lambda p: (os.path.basename(p).casefold(), p))


def sbs_icon(path):
    """PNG de la première icône de graphe d'un .sbs (XML), None sinon."""
    with open(path, "rb") as f:
        data = f.read(SBS_ICON_BYTES)
    m = SBS_ICON.search(data)
    if m is None:
        return None
    try:
        png = base64.b64decode(m.group(1))
    except ValueError:
        return None
    return png if png[:8] == b"\x89PNG\r\n\x1a\n" else None


def icon_cache():
    return os.path.join(subzero_render.CACHE_DIR, "browser")


def load_icon(path):
    """QImage de l'icône du paquet (ICON × 2 au plus), None s'il n'en a pas ; lue une fois, puis dans le cache."""
    try:
        st = os.stat(path)
    except OSError:
        return None
    key = hashlib.sha1(("%s|%d|%d" % (path, st.st_mtime_ns, st.st_size)).encode("utf-8")).hexdigest()
    cached = os.path.join(icon_cache(), key)
    if os.path.isfile(cached + ".png"):
        image = QtGui.QImage(cached + ".png")
        if not image.isNull():
            return image
    if os.path.isfile(cached + ".none"):
        return None
    try:
        data = subzero_meta.package_icon(path) if path.lower().endswith(".sbsar") else sbs_icon(path)
    except Exception as e:  # noqa: BLE001 — archive abîmée, XML inattendu : pas d'icône
        log("browser: icon of %s: %s" % (os.path.basename(path), e))
        data = None
    image = QtGui.QImage.fromData(data) if data else QtGui.QImage()
    try:
        os.makedirs(icon_cache(), exist_ok=True)
        if image.isNull():
            open(cached + ".none", "w").close()
            return None
        image = image.scaled(ICON * 2, ICON * 2, QtCore.Qt.KeepAspectRatio, QtCore.Qt.SmoothTransformation)
        image.save(cached + ".tmp.png", "PNG")
        os.replace(cached + ".tmp.png", cached + ".png")
    except OSError as e:
        log("browser: icon cache: %s" % e)
    return None if image.isNull() else image


# ---- Arrière-plan -------------------------------------------------------------------------------------------------------


class Scanner(QtCore.QObject):
    """Un fil : listes des dossiers d'abord, puis icônes, celles de l'onglet courant avant les autres."""

    listed = QtCore.Signal(str, object, str)  # dossier, [chemins], erreur
    iconed = QtCore.Signal(str, object)  # chemin, QImage ou None

    def __init__(self):
        super().__init__()
        self._cond = threading.Condition()
        self._scans = collections.deque()
        self._icons = {}  # dossier -> deque de chemins
        self._current = None
        self._quit = False
        self._busy = False
        threading.Thread(target=self._loop, name="subzero-browser", daemon=True).start()

    def scan(self, folder):
        with self._cond:
            if folder not in self._scans:
                self._scans.append(folder)
            self._cond.notify()

    def prefer(self, folder):
        with self._cond:
            self._current = folder

    def stop(self):
        with self._cond:
            self._quit = True
            self._cond.notify()

    def wait(self, timeout=30.0):
        with self._cond:
            self._cond.wait_for(lambda: not self._busy and not self._scans and not any(self._icons.values()), timeout)

    def _next(self):
        if self._scans:
            return "scan", self._scans.popleft()
        order = [self._current] + [f for f in self._icons if f != self._current]
        for folder in order:
            queue = self._icons.get(folder)
            if queue:
                return "icon", queue.popleft()
        return None

    def _loop(self):
        while True:
            with self._cond:
                self._cond.wait_for(lambda: self._quit or self._scans or any(self._icons.values()))
                if self._quit:
                    return
                task = self._next()
                self._busy = task is not None
            if task is None:
                continue
            kind, arg = task
            if kind == "scan":
                try:
                    paths, error = scan(arg), ""
                except OSError as e:
                    paths, error = [], str(e)
                with self._cond:
                    self._icons[arg] = collections.deque(paths)
                self.listed.emit(arg, paths, error)
            else:
                self.iconed.emit(arg, load_icon(arg))
            with self._cond:
                self._busy = False
                self._cond.notify_all()


# ---- Panneau ------------------------------------------------------------------------------------------------------------


def placeholder(ext):
    """Icône d'un paquet sans icône : tuile sombre et extension."""
    pix = QtGui.QPixmap(ICON * 2, ICON * 2)
    pix.fill(QtGui.QColor("#262626"))
    p = QtGui.QPainter(pix)
    p.setPen(QtGui.QColor("#6e6e6e"))
    font = p.font()
    font.setPixelSize(28)
    font.setBold(True)
    p.setFont(font)
    p.drawText(pix.rect(), QtCore.Qt.AlignCenter, ext.upper())
    p.end()
    pix.setDevicePixelRatio(2.0)
    return QtGui.QIcon(pix)


class PackageList(QtWidgets.QListWidget):
    """Grille d'icônes ; glisser donne l'URL du fichier."""

    def __init__(self):
        super().__init__()
        self.setViewMode(QtWidgets.QListView.IconMode)
        self.setResizeMode(QtWidgets.QListView.Adjust)
        self.setMovement(QtWidgets.QListView.Static)
        self.setIconSize(QtCore.QSize(ICON, ICON))
        self.setGridSize(GRID)
        self.setWordWrap(True)
        self.setUniformItemSizes(True)
        self.setTextElideMode(QtCore.Qt.ElideMiddle)
        self.setSelectionMode(QtWidgets.QAbstractItemView.SingleSelection)
        self.setDragEnabled(True)
        self.setDragDropMode(QtWidgets.QAbstractItemView.DragOnly)
        self.setDefaultDropAction(QtCore.Qt.CopyAction)
        self.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        self.setFrameShape(QtWidgets.QFrame.NoFrame)
        self.setSpacing(0)

    def mimeTypes(self):
        return ["text/uri-list"]

    def mimeData(self, items):
        data = QtCore.QMimeData()
        data.setUrls([QtCore.QUrl.fromLocalFile(i.data(PATH_ROLE)) for i in items])
        return data


class Browser(QtWidgets.QFrame):
    """opened(chemin) : paquet choisi (double clic, Entrée, menu)."""

    opened = QtCore.Signal(str)

    def __init__(self):
        super().__init__()
        self.setObjectName("browser")
        self.setFixedWidth(WIDTH)
        self.config = load_config()
        self.tabs = []  # [{"path", "removable", "paths", "error"}]
        self.icons = {}  # chemin -> QIcon
        self.placeholders = {ext: placeholder(ext.lstrip(".")) for ext in EXTENSIONS}
        self.marked = None
        self.scanner = Scanner()
        self.scanner.listed.connect(self._on_listed)
        self.scanner.iconed.connect(self._on_icon)

        col = QtWidgets.QVBoxLayout(self)
        col.setContentsMargins(0, 0, 0, 0)
        col.setSpacing(0)
        top = QtWidgets.QFrame()
        top.setObjectName("bar")
        head = QtWidgets.QVBoxLayout(top)
        head.setContentsMargins(10, 10, 10, 8)
        head.setSpacing(8)
        row = QtWidgets.QHBoxLayout()
        row.setSpacing(4)
        row.addWidget(section_title("Library"))
        row.addStretch(1)
        self.add_button = QtWidgets.QToolButton()
        self.add_button.setObjectName("tool")
        self.add_button.setText("+")
        self.add_button.setToolTip("Add a folder: a new tab, kept for next time")
        self.refresh_button = QtWidgets.QToolButton()
        self.refresh_button.setObjectName("tool")
        self.refresh_button.setText("↻")
        self.refresh_button.setToolTip("Scan the folder of this tab again")
        row.addWidget(self.add_button)
        row.addWidget(self.refresh_button)
        head.addLayout(row)
        self.search = QtWidgets.QLineEdit()
        self.search.setPlaceholderText("Search")
        self.search.setClearButtonEnabled(True)
        head.addWidget(self.search)
        col.addWidget(top)
        self.tab_bar = QtWidgets.QTabBar()
        self.tab_bar.setObjectName("library")
        self.tab_bar.setExpanding(False)
        self.tab_bar.setUsesScrollButtons(True)
        self.tab_bar.setElideMode(QtCore.Qt.ElideRight)
        self.tab_bar.setDocumentMode(True)
        self.tab_bar.setDrawBase(False)
        self.tab_bar.setTabsClosable(True)
        self.tab_bar.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        col.addWidget(self.tab_bar)
        self.stack = QtWidgets.QStackedWidget()
        self.list = PackageList()
        self.empty = QtWidgets.QLabel()
        self.empty.setObjectName("muted")
        self.empty.setWordWrap(True)
        self.empty.setAlignment(QtCore.Qt.AlignCenter)
        self.empty.setContentsMargins(16, 16, 16, 16)
        self.stack.addWidget(self.list)
        self.stack.addWidget(self.empty)
        col.addWidget(self.stack, 1)
        foot = QtWidgets.QFrame()
        foot.setObjectName("bar")
        bottom = QtWidgets.QHBoxLayout(foot)
        bottom.setContentsMargins(10, 5, 10, 5)
        self.count = QtWidgets.QLabel()
        self.count.setObjectName("caption")
        bottom.addWidget(self.count, 1)
        col.addWidget(foot)

        self.add_button.clicked.connect(self._pick_folder)
        self.refresh_button.clicked.connect(self.refresh)
        self.search.textChanged.connect(self._filter)
        self.tab_bar.currentChanged.connect(self._on_tab)
        self.tab_bar.tabCloseRequested.connect(self.remove_tab)
        self.tab_bar.customContextMenuRequested.connect(self._tab_menu)
        self.list.itemActivated.connect(lambda item: self.opened.emit(item.data(PATH_ROLE)))
        self.list.customContextMenuRequested.connect(self._item_menu)

        for path in library_folders():
            self._add_tab(path, removable=False)
        for path in self.config.get("folders") or []:
            if isinstance(path, str) and path and self._tab_index(path) < 0:
                self._add_tab(os.path.abspath(os.path.expanduser(path)), removable=True)
        start = self._tab_index(self.config.get("tab") or "")
        if self.tabs:
            self.tab_bar.setCurrentIndex(max(0, start))
            self._on_tab(self.tab_bar.currentIndex())
        else:
            self._show_empty()

    # -- onglets

    def _tab_index(self, path):
        return next((i for i, t in enumerate(self.tabs) if t["path"] == path), -1)

    def _add_tab(self, path, removable):
        self.tabs.append({"path": path, "removable": removable, "paths": None, "error": ""})
        label = os.path.basename(path.rstrip(os.sep)) or path
        self.tab_bar.blockSignals(True)
        i = self.tab_bar.addTab(label)
        self.tab_bar.blockSignals(False)
        self.tab_bar.setTabToolTip(i, path + ("" if removable else "\n($SUBZERO_LIBRARY)"))
        if not removable:  # dossiers de l'environnement : pas de croix
            self.tab_bar.setTabButton(i, QtWidgets.QTabBar.RightSide, None)
            self.tab_bar.setTabButton(i, QtWidgets.QTabBar.LeftSide, None)
        return i

    def add_folder(self, path):
        """Onglet du dossier (créé et gardé dans browser.json s'il n'y est pas), puis choisi."""
        path = os.path.abspath(os.path.expanduser(path))
        i = self._tab_index(path)
        if i < 0:
            i = self._add_tab(path, removable=True)
            self._save()
        if self.tab_bar.currentIndex() == i:
            self._on_tab(i)
        else:
            self.tab_bar.setCurrentIndex(i)  # currentChanged -> _on_tab

    def remove_tab(self, index):
        if not (0 <= index < len(self.tabs)) or not self.tabs[index]["removable"]:
            return
        del self.tabs[index]
        self.tab_bar.removeTab(index)  # currentChanged choisit le voisin
        self._save()
        if not self.tabs:
            self._show_empty()

    def _pick_folder(self):
        start = self.tabs[self.tab_bar.currentIndex()]["path"] if self.tabs else os.path.expanduser("~")
        path = QtWidgets.QFileDialog.getExistingDirectory(self, "Add a folder to the library", start)
        if path:
            self.add_folder(path)

    def _save(self):
        self.config["folders"] = [t["path"] for t in self.tabs if t["removable"]]
        current = self.current_tab()
        self.config["tab"] = current["path"] if current else ""
        save_config(self.config)

    def remember_visible(self, visible):
        if self.config.get("visible") != bool(visible):
            self.config["visible"] = bool(visible)
            save_config(self.config)

    def current_tab(self):
        i = self.tab_bar.currentIndex()
        return self.tabs[i] if 0 <= i < len(self.tabs) else None

    def _on_tab(self, index):
        tab = self.current_tab()
        if tab is None:
            return
        self.scanner.prefer(tab["path"])
        if tab["paths"] is None:
            self._show_message("Scanning %s..." % tab["path"])
            self.scanner.scan(tab["path"])
        else:
            self._fill(tab)
        if self.config.get("tab") != tab["path"]:
            self._save()

    def refresh(self):
        tab = self.current_tab()
        if tab is not None:
            tab["paths"] = None
            self._on_tab(self.tab_bar.currentIndex())

    def _on_listed(self, folder, paths, error):
        for tab in self.tabs:
            if tab["path"] == folder:
                tab["paths"], tab["error"] = paths, error
        current = self.current_tab()
        if current is not None and current["path"] == folder:
            self._fill(current)

    # -- liste

    def _show_empty(self):
        self._show_message("No library folder.\nSet SUBZERO_LIBRARY to your .sbsar folders, or add one with +.")
        self.count.setText("")

    def _show_message(self, text, error=False):
        self.empty.setText(text)
        self.empty.setStyleSheet("color: %s;" % (ERROR if error else MUTED))
        self.stack.setCurrentWidget(self.empty)

    def _fill(self, tab):
        self.list.clear()
        if tab["error"]:
            self._show_message(tab["error"], error=True)
            self.count.setText("")
            return
        if not tab["paths"]:
            self._show_message("No .sbsar or .sbs file in\n%s" % tab["path"])
            self.count.setText("")
            return
        root = tab["path"]
        for path in tab["paths"]:
            name, ext = os.path.splitext(os.path.basename(path))
            item = QtWidgets.QListWidgetItem(self.icons.get(path) or self.placeholders[ext.lower()], name)
            item.setData(PATH_ROLE, path)
            sub = os.path.relpath(os.path.dirname(path), root)
            item.setToolTip("%s%s\n%s" % (os.path.basename(path), "" if sub == "." else "\n" + sub, path))
            item.setSizeHint(GRID)
            self.list.addItem(item)
        self.stack.setCurrentWidget(self.list)
        more = " (first %d)" % MAX_ITEMS if len(tab["paths"]) >= MAX_ITEMS else ""
        self.count.setText("%d package%s%s" % (len(tab["paths"]), "" if len(tab["paths"]) == 1 else "s", more))
        self.count.setToolTip(root)
        self._filter(self.search.text())
        self.mark(self.marked)

    def _on_icon(self, path, image):
        if image is None:
            return
        pix = QtGui.QPixmap.fromImage(image)
        pix.setDevicePixelRatio(2.0)
        icon = QtGui.QIcon(pix)
        self.icons[path] = icon
        for i in range(self.list.count()):
            item = self.list.item(i)
            if item.data(PATH_ROLE) == path:
                item.setIcon(icon)
                break

    def _filter(self, text):
        words = text.casefold().split()
        shown = 0
        for i in range(self.list.count()):
            item = self.list.item(i)
            name = os.path.basename(item.data(PATH_ROLE)).casefold()
            hidden = not all(w in name for w in words)
            item.setHidden(hidden)
            shown += not hidden
        tab = self.current_tab()
        if tab is not None and tab["paths"] and words:
            self.count.setText("%d of %d packages" % (shown, len(tab["paths"])))
        elif tab is not None and tab["paths"]:
            self.count.setText("%d package%s" % (len(tab["paths"]), "" if len(tab["paths"]) == 1 else "s"))

    def mark(self, path):
        """Paquet ouvert dans la fenêtre : sélectionné dans la liste s'il y est."""
        self.marked = path
        for i in range(self.list.count()):
            item = self.list.item(i)
            if path and item.data(PATH_ROLE) == path:
                self.list.setCurrentItem(item)
                self.list.scrollToItem(item)
                return
        self.list.clearSelection()

    # -- menus

    def _item_menu(self, pos):
        item = self.list.itemAt(pos)
        if item is None:
            return
        path = item.data(PATH_ROLE)
        menu = QtWidgets.QMenu(self)
        menu.addAction("Open", lambda: self.opened.emit(path))
        menu.addAction("Show in Folder", lambda: reveal(path))
        menu.addAction("Copy Path", lambda: QtWidgets.QApplication.clipboard().setText(path))
        menu.exec(self.list.viewport().mapToGlobal(pos))

    def _tab_menu(self, pos):
        index = self.tab_bar.tabAt(pos)
        if index < 0:
            return
        tab = self.tabs[index]
        menu = QtWidgets.QMenu(self)
        menu.addAction("Show Folder", lambda: QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(tab["path"])))
        menu.addAction("Refresh", lambda: (self.tab_bar.setCurrentIndex(index), self.refresh()))
        remove = menu.addAction("Remove Tab", lambda: self.remove_tab(index))
        remove.setEnabled(tab["removable"])
        if not tab["removable"]:
            remove.setToolTip("From $SUBZERO_LIBRARY")
        menu.exec(self.tab_bar.mapToGlobal(pos))

    # -- fenêtre

    def stop(self):
        self.scanner.stop()

    def flush(self, timeout=30.0):
        """Essais : listes et icônes finies, puis livrées."""
        QtWidgets.QApplication.processEvents()
        self.scanner.wait(timeout)
        QtWidgets.QApplication.processEvents()

    def find(self, name):
        """Chemin du paquet de ce nom (sans extension) dans l'onglet courant, None sinon."""
        tab = self.current_tab() or {}
        for path in tab.get("paths") or []:
            if os.path.splitext(os.path.basename(path))[0] == name:
                return path
        return None

    def test_log(self):
        visible = [self.list.item(i).text() for i in range(self.list.count()) if not self.list.item(i).isHidden()]
        return {"visible": self.isVisible(), "tabs": [{"path": t["path"], "label": self.tab_bar.tabText(i),
                                                      "removable": t["removable"], "count": len(t["paths"] or []),
                                                      "error": t["error"]} for i, t in enumerate(self.tabs)],
                "current": (self.current_tab() or {}).get("path"), "items": visible,
                "icons": sorted(os.path.basename(p) for p in self.icons), "count": self.count.text(),
                "message": self.empty.text() if self.stack.currentWidget() is self.empty else "",
                "config": dict(self.config), "config_file": os.path.join(config_dir(), CONFIG)}


def reveal(path):
    """Montre le fichier dans le Finder (sélectionné), sinon ouvre son dossier."""
    if sys.platform == "darwin":
        subprocess.Popen(["open", "-R", path])
    else:
        QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(os.path.dirname(path)))
