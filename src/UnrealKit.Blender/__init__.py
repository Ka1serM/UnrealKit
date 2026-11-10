import queue
import time
import traceback

import bpy

from . import deferred_update, session
from .product import NAME
from .builders import SceneBuilder
from .server import HOST, PORT, Server

PROGRESS_INTERVAL = 0.25
#! Each return to Blender costs its idle sleep, so the import waits a little for the next record.
MESSAGE_WAIT = 0.005

server = None
active = []
last_result = None


class Pending:

    def __init__(self, connection):
        self.connection = connection
        self.session = session.Session(SceneBuilder)
        self.progress_sent = 0.0
        self.progress_last = None


def send_progress(pending, now):
    current = (pending.session.built, pending.session.received)
    if not pending.session.received or now - pending.progress_sent < PROGRESS_INTERVAL or current == pending.progress_last:
        return
    pending.progress_sent = now
    pending.progress_last = current
    pending.connection.replies.put(session.progress(*current))


def end(pending, state, reply, diagnostics=()):
    global last_result
    last_result = (pending.session.title, state, list(diagnostics))
    pending.connection.finish(reply)


def advance(pending):
    connection = pending.connection
    while True:
        send_progress(pending, time.perf_counter())
        try:
            message = connection.messages.get(timeout=MESSAGE_WAIT)
        except queue.Empty:
            return
        kind, record = session.decode(message)
        reply = pending.session.accept(kind, record)
        state = pending.session.state
        if state == "finished":
            builder = pending.session.builder
            connection.replies.put(session.progress(pending.session.built, pending.session.received))
            end(pending, "Finished", reply, builder.diagnostics)
            print(f"[{NAME}] Built {pending.session.title} with {len(builder.diagnostics)} diagnostics")
            return
        if state == "stopped":
            end(pending, "Stopped by the app", None)
            return
        if reply is not None:
            connection.replies.put(reply)


def import_handler():
    if server is None:
        return None
    while True:
        try:
            active.append(Pending(server.connections.get_nowait()))
        except queue.Empty:
            break
    for pending in list(active):
        if pending.connection.done.is_set():
            active.remove(pending)
            continue
        if pending.connection.closed.is_set() and pending.connection.messages.empty():
            if pending.session.builder is not None:
                pending.session.builder.finish()
            end(pending, "The app closed the connection", None)
            active.remove(pending)
            continue
        try:
            advance(pending)
        except session.ProtocolError as error:
            print(f"[{NAME}] {error}")
            end(pending, "Failed", session.failed("import.protocol", str(error)))
        except Exception as error:
            traceback.print_exc()
            end(pending, "Failed", session.failed("import.failed", str(error)))
    return 0.0 if active else 0.05


def register():
    global server
    deferred_update.register()
    server = Server(session.hello, session.failed)
    server.start()
    bpy.app.timers.register(import_handler, first_interval=0.05, persistent=True)


def unregister():
    global server
    if bpy.app.timers.is_registered(import_handler):
        bpy.app.timers.unregister(import_handler)
    if server is not None:
        server.stop()
        server = None
    active.clear()
    deferred_update.unregister()
