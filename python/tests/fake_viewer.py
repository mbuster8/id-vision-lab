"""A stand-in for the viewer: a ZeroMQ socket on a thread that records each request and answers as told."""

import json
import socket
import threading

import zmq

OK = b'{"status":"ok"}'
ERROR = b'{"status":"error","reason":"rows out of range"}'


def free_port() -> int:
    """A loopback port no one was listening on a moment ago."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class FakeViewer:
    """Binds to `endpoint` (an ephemeral loopback port by default) and answers each request by `reply`: "ok",
    "error", "garbage" (not JSON) or "silent" (never). A ROUTER socket, so a silent viewer still takes the next
    socket's request, as the viewer's queue does. `received` holds (header dict, pixel bytes) per request."""

    def __init__(self, reply: str = "ok", endpoint: str = "tcp://127.0.0.1:*"):
        self.reply = reply
        self.received = []
        self._context = zmq.Context()
        self._socket = self._context.socket(zmq.ROUTER)
        self._socket.setsockopt(zmq.LINGER, 0)
        self._socket.bind(endpoint)
        self.endpoint = self._socket.getsockopt(zmq.LAST_ENDPOINT).decode()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *_):
        self._stop.set()
        self._thread.join()
        self._socket.close(linger=0)
        self._context.term()

    def _serve(self):
        while not self._stop.is_set():
            if not self._socket.poll(20, zmq.POLLIN):
                continue
            identity, empty, header, pixels = self._socket.recv_multipart()
            self.received.append((json.loads(header), pixels))
            answer = {"ok": OK, "error": ERROR, "garbage": b"not json"}.get(self.reply)
            if answer is not None:
                self._socket.send_multipart([identity, empty, answer])
