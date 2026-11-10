import base64
import hashlib
import queue
import select
import socket
import struct
import threading
import traceback

import numpy as np

from .product import NAME, PORT

HOST = "127.0.0.1"
PATH = "/scene"
_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def _receive_into(sock, view):
    while len(view):
        received = sock.recv_into(view)
        if not received:
            raise ConnectionError("the app closed the connection")
        view = view[received:]


def _receive(sock, count):
    data = bytearray(count)
    _receive_into(sock, memoryview(data))
    return data


def _receive_payload(sock, buffer, offset, length, mask):
    _receive_into(sock, memoryview(buffer)[offset:offset + length])
    words = length // 8
    key = bytes(mask) * 2
    np.frombuffer(buffer, np.uint64, words, offset)[:] ^= np.frombuffer(key, np.uint64)
    tail = length - words * 8
    if tail:
        np.frombuffer(buffer, np.uint8, tail, offset + words * 8)[:] ^= np.frombuffer(key, np.uint8)[:tail]


def _read_message(sock):
    message = None
    message_opcode = None
    while True:
        first, second = _receive(sock, 2)
        fin = bool(first & 0x80)
        opcode = first & 0x0F
        if first & 0x70:
            raise ValueError("WebSocket extensions are not negotiated")
        if not second & 0x80:
            raise ValueError("a client frame must be masked")
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", _receive(sock, 2))[0]
        elif length == 127:
            length = struct.unpack("!Q", _receive(sock, 8))[0]
        if opcode >= 8 and (not fin or length > 125):
            raise ValueError("invalid control frame")
        mask = _receive(sock, 4)
        if opcode >= 8:
            payload = bytearray(length)
            _receive_payload(sock, payload, 0, length, mask)
            if opcode == 9:
                _send_frame(sock, payload, opcode=10)
                continue
            if opcode == 10:
                continue
            return opcode, payload
        if opcode == 0:
            if message is None:
                raise ValueError("unexpected continuation frame")
        elif opcode in (1, 2):
            if message is not None:
                raise ValueError("interleaved messages")
            message = bytearray()
            message_opcode = opcode
        else:
            raise ValueError(f"unknown opcode {opcode}")
        offset = len(message)
        if offset:
            message.extend(bytes(length))
        else:
            message = bytearray(length)
        _receive_payload(sock, message, offset, length, mask)
        if fin:
            return message_opcode, message


def _send_frame(sock, payload, opcode=2):
    header = bytearray([0x80 | opcode])
    length = len(payload)
    if length < 126:
        header.append(length)
    elif length <= 0xFFFF:
        header.extend((126, *struct.pack("!H", length)))
    else:
        header.append(127)
        header.extend(struct.pack("!Q", length))
    sock.sendall(header)
    sock.sendall(payload)


def _close(sock):
    try:
        _send_frame(sock, b"", opcode=8)
        sock.settimeout(1.0)
        while True:
            opcode, _ = _read_message(sock)
            if opcode == 8:
                break
    except (OSError, ValueError, ConnectionError):
        pass


class Connection:

    def __init__(self):
        self.messages = queue.Queue(maxsize=16)
        self.replies = queue.Queue()
        self.closed = threading.Event()
        self.done = threading.Event()
        self.session = None

    def finish(self, reply=None):
        if reply is not None:
            self.replies.put(reply)
        self.done.set()


class Server(threading.Thread):
    def __init__(self, hello, failed):
        super().__init__(daemon=True, name="Scene WebSocket")
        self.hello = hello
        self.failed = failed
        self.connections = queue.Queue()
        self.stopping = threading.Event()
        self.listener = None
        self.error = None

    def stop(self):
        self.stopping.set()
        if self.listener:
            self.listener.close()
        if self.is_alive() and threading.current_thread() is not self:
            self.join(timeout=2.0)

    def run(self):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
                self.listener = listener
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind((HOST, PORT))
                listener.listen(4)
                listener.settimeout(0.5)
                print(f"[{NAME}] Listening on ws://{HOST}:{PORT}{PATH}")
                while not self.stopping.is_set():
                    try:
                        client, _ = listener.accept()
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    threading.Thread(target=self._serve, args=(client,), daemon=True).start()
        except OSError as error:
            self.error = str(error)
            print(f"[{NAME}] Cannot listen on {HOST}:{PORT}: {error}")

    def _serve(self, sock):
        connection = None
        with sock:
            try:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                sock.settimeout(30.0)
                request = bytearray()
                while b"\r\n\r\n" not in request:
                    request.extend(_receive(sock, 1))
                    if len(request) > 16384:
                        raise ValueError("the request headers are too large")
                lines = request.decode("latin1").split("\r\n")
                if not lines[0].startswith(f"GET {PATH} "):
                    raise ValueError("unknown WebSocket path")
                headers = dict(line.split(":", 1) for line in lines[1:] if ":" in line)
                headers = {key.strip().lower(): value.strip() for key, value in headers.items()}
                key = headers.get("sec-websocket-key")
                if not key:
                    raise ValueError("no Sec-WebSocket-Key")
                accept = base64.b64encode(hashlib.sha1((key + _GUID).encode("ascii")).digest()).decode("ascii")
                sock.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode("ascii"))
                connection = Connection()
                self.connections.put(connection)
                _send_frame(sock, self.hello())
                while not self.stopping.is_set():
                    done = connection.done.is_set()
                    self._send_replies(sock, connection)
                    if done:
                        _close(sock)
                        break
                    if not select.select([sock], [], [], 0.02)[0]:
                        continue
                    opcode, message = _read_message(sock)
                    if opcode == 8:
                        break
                    if opcode != 2:
                        raise ValueError("expected a binary message")
                    while not self.stopping.is_set() and not connection.done.is_set():
                        try:
                            connection.messages.put(message, timeout=0.05)
                            break
                        except queue.Full:
                            self._send_replies(sock, connection)
            except Exception as error:
                traceback.print_exc()
                try:
                    _send_frame(sock, self.failed("import.connection", str(error)))
                    _close(sock)
                except Exception:
                    pass
            finally:
                if connection is not None:
                    connection.closed.set()

    @staticmethod
    def _send_replies(sock, connection):
        while True:
            try:
                _send_frame(sock, connection.replies.get_nowait())
            except queue.Empty:
                return
