# Faux sidecar ShotGrid pour le banc du superviseur (sg_super_test.c) : parle le protocole de studio_sg_bridge.py et
# se comporte mal sur commande. FAKE_HELLO choisit le démarrage : ok (défaut), noise, fail, bad_protocol, exit, silent.
import json
import os
import sys
import time

out = sys.stdout


def send(message):
    out.write(json.dumps(message, ensure_ascii=True, separators=(",", ":")) + "\n")
    out.flush()


def log(text):
    sys.stderr.write(text + "\n")
    sys.stderr.flush()


hello = os.environ.get("FAKE_HELLO", "ok")
log("fake sidecar %d starting (%s)" % (os.getpid(), hello))
if hello == "exit":
    log("Traceback: ImportError: no module named sgtk")
    sys.exit(4)
if hello == "silent":
    time.sleep(60)
if hello == "noise":
    out.write("Welcome to Python!\n")
if hello == "fail":
    send({"id": 0, "ok": False, "error": {"code": "internal", "message": "sidecar startup failed: boom"}})
    sys.exit(1)
send({"id": 0, "ok": True, "data": {"protocol": 2 if hello == "bad_protocol" else 1,
                                   "toolkit": {"available": False, "core": None, "error": "fake"}}})

project = None
for line in sys.stdin:
    request = json.loads(line)
    rid, op, args = request["id"], request["op"], request["args"]
    if op == "echo":
        send({"id": rid, "ok": True, "data": args})
    elif op == "pid":
        send({"id": rid, "ok": True, "data": os.getpid()})
    elif op == "sleep":
        time.sleep(args["s"])
        send({"id": rid, "ok": True, "data": args["s"]})
    elif op == "crash":
        log("boom: the fake sidecar crashes")
        os._exit(3)
    elif op == "project":
        if project is not None and project != args["p"]:
            send({"id": rid, "ok": False, "restart": True,
                  "error": {"code": "restart_required", "message": "switching to project %d" % args["p"]}})
            break
        project = args["p"]
        send({"id": rid, "ok": True, "data": {"project": project, "pid": os.getpid()}})
    elif op == "always_restart":
        send({"id": rid, "ok": False, "restart": True, "error": {"code": "restart_required", "message": "again"}})
        break
    elif op == "stale":
        send({"id": rid - 1, "ok": True, "data": "stale"})
        out.write("this is not JSON\n")
        send({"id": rid, "ok": True, "data": "fresh"})
    elif op == "log":
        for i in range(args["n"]):
            log("line %d" % i)
        sys.stderr.write("partial without newline")
        sys.stderr.flush()
        send({"id": rid, "ok": True, "data": None})
    elif op == "huge":
        send({"id": rid, "ok": True, "data": "x" * args["n"]})
    elif op == "close_stdout":
        out.close()
        os.close(1)
        time.sleep(30)
    else:
        send({"id": rid, "ok": False, "error": {"code": "unknown_op", "message": op}})
log("fake sidecar %d exiting" % os.getpid())
