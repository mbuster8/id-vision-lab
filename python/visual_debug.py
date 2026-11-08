"""Send numpy images to the Visual Debug Viewer.

A pure-Python sender for the viewer's wire format, version 2 (docs/wire-format.md), written to send exactly what the
C++ visual_debug library sends: for the shared golden cases (tests/wire/header_cases.json) the very same header bytes,
and for any input a header that reads back as the same values.

    import visual_debug as vd
    vd.send_image(image, "Sobel X")               # a numpy array, BGR as OpenCV holds it; rgb=True to swap
    vd.send_live(frame, "camera", overlays=[vd.Rectangle(10, 20, 100, 50, label="face")])

Nothing about the image or the viewer raises: each send returns a SendResult. With no viewer running, the first send
waits once for the reply timeout and later ones return at once until the retry interval has passed.
"""

from __future__ import annotations

import dataclasses
import enum
import json
import math
import numbers
import os
import re
import threading
import time

import numpy as np
import zmq

# The wire format's limits (libs/visual_debug/include/visual_debug/wire_format.h).
PROTOCOL_VERSION = 2
MAX_NAME_BYTES = 1024
MAX_HEADER_BYTES = 64 * 1024
MAX_PIXEL_BYTES = 512 * 1024 * 1024
MAX_DIMENSION = 32768
MAX_CHANNELS = 4
MAX_SENT_AT_MILLISECONDS = 253402300799999  # the end of the year 9999
MAX_OVERLAYS = 256
MAX_POLYGON_POINTS = 1024
MAX_LABEL_BYTES = 64
MAX_OVERLAY_COORDINATE = 1_000_000.0
_LARGEST_EXACT_INTEGER = 2.0**53

DEFAULT_ENDPOINT = "tcp://127.0.0.1:7139"

OVERLAY_GREEN = "#78E678"  # every overlay's colour unless it names another
OVERLAY_RED = "#E05050"
OVERLAY_AMBER = "#F0B040"

# --- The wire format's number and text rules, as libs/visual_debug/src/wire_format.cpp applies them -----------------


def _format_number(value: float) -> str:
    """A finite, non-whole double as nlohmann::json writes it.

    The digits are repr()'s, the shortest that read back as the same double; the layout is nlohmann's (format_buffer
    in to_chars.hpp): with k digits and the decimal point n places in, fixed notation for 0 < n <= 15, "0.000ddd" for
    -4 < n <= 0, else "d.ddde+XX" with at least two exponent digits. nlohmann finds its digits with Grisu2, which on a
    rare 17-digit value can end in another digit that reads back as the same double.
    """
    value = float(value)
    sign = "-" if math.copysign(1.0, value) < 0 else ""
    mantissa, _, exponent_text = repr(abs(value)).partition("e")
    whole, _, fraction = mantissa.partition(".")
    combined = whole + fraction
    digits = combined.lstrip("0")
    point = len(whole) + (int(exponent_text) if exponent_text else 0) - (len(combined) - len(digits))
    digits = digits.rstrip("0") or "0"
    k, n = len(digits), point
    if k <= n <= 15:
        text = digits + "0" * (n - k) + ".0"
    elif 0 < n <= 15:
        text = digits[:n] + "." + digits[n:]
    elif -4 < n <= 0:
        text = "0." + "0" * -n + digits
    else:
        exponent = n - 1
        text = (
            digits[0]
            + ("." + digits[1:] if k > 1 else "")
            + ("e-" if exponent < 0 else "e+")
            + f"{abs(exponent):02d}"
        )
    return sign + text


def _round_thousandth(value) -> float:
    """value to a thousandth, as std::round(value * 1000) / 1000: halves away from zero, in double precision.

    Not round(), which takes halves to even, nor floor(x + 0.5), wrong for 0.49999999999999994. A value that is not
    finite, or whose thousandths overflow, comes back not finite, for the caller's limit check to leave out.
    """
    scaled = float(value) * 1000.0
    if not math.isfinite(scaled):
        return scaled
    rounded = float(math.trunc(scaled))
    if abs(scaled - rounded) >= 0.5:
        rounded += math.copysign(1.0, scaled)
    return rounded / 1000.0


def _whole_or_float(value, limit: float) -> int | float:
    """An int where C++ writes an integer (a whole number within `limit`), else the float itself."""
    value = float(value)
    if math.isfinite(value) and value == math.floor(value) and abs(value) <= limit:
        return int(value)
    return value


_SURROGATE = re.compile("[\ud800-\udfff]")


def _valid_text(text: str) -> str:
    """Each lone surrogate, the only invalid UTF-8 a Python string can hold, becomes one U+FFFD."""
    return _SURROGATE.sub("�", text)


def _cut_utf8(text: str, max_bytes: int) -> str:
    """text cut to at most max_bytes of UTF-8, at a character boundary."""
    encoded = text.encode("utf-8")
    if len(encoded) <= max_bytes:
        return text
    return encoded[:max_bytes].decode("utf-8", errors="ignore")


def _json_string(text: str) -> str:
    """A JSON string as nlohmann writes it: Python's json escapes the same characters with ensure_ascii off."""
    return json.dumps(text, ensure_ascii=False)


def _number_text(value, limit: float) -> str:
    """A number as C++ writes it: an integer when whole and within `limit`, else nlohmann's layout of the double."""
    number = _whole_or_float(value, limit)
    return str(number) if isinstance(number, int) else _format_number(number)


# --- Overlays (docs/wire-format.md, Overlays) ------------------------------------------------------------------------

_COLOUR = re.compile("#[0-9A-Fa-f]{6}")


def _checked_label_and_colour(label, colour) -> None:
    if not isinstance(label, str):
        raise TypeError("an overlay's label is a str")
    if not isinstance(colour, str) or _COLOUR.fullmatch(colour) is None:
        raise ValueError(f'an overlay\'s colour is "#RRGGBB", not {colour!r}')


def _as_float(owner, *names) -> None:
    for name in names:
        object.__setattr__(owner, name, float(getattr(owner, name)))


@dataclasses.dataclass(frozen=True)
class Polygon:
    """A closed shape: points in image pixels, x to the right and y down; a list of pairs, an (N, 2) array, or an
    OpenCV contour (N, 1, 2)."""

    points: tuple
    label: str = ""
    colour: str = OVERLAY_GREEN

    def __post_init__(self):
        pairs = np.asarray(self.points, dtype=np.float64).reshape(-1, 2)
        object.__setattr__(self, "points", tuple((float(x), float(y)) for x, y in pairs))
        _checked_label_and_colour(self.label, self.colour)


@dataclasses.dataclass(frozen=True)
class Rectangle:
    """An axis-aligned rectangle: its top-left corner, width and height."""

    x: float
    y: float
    width: float
    height: float
    label: str = ""
    colour: str = OVERLAY_GREEN

    def __post_init__(self):
        _as_float(self, "x", "y", "width", "height")
        _checked_label_and_colour(self.label, self.colour)


@dataclasses.dataclass(frozen=True)
class Circle:
    """A circle: its centre and radius."""

    x: float
    y: float
    radius: float
    label: str = ""
    colour: str = OVERLAY_GREEN

    def __post_init__(self):
        _as_float(self, "x", "y", "radius")
        _checked_label_and_colour(self.label, self.colour)


@dataclasses.dataclass(frozen=True)
class Point:
    """A dot of a fixed size on screen."""

    x: float
    y: float
    label: str = ""
    colour: str = OVERLAY_GREEN

    def __post_init__(self):
        _as_float(self, "x", "y")
        _checked_label_and_colour(self.label, self.colour)


_SHAPES = (Polygon, Rectangle, Circle, Point)


def _rounded_within_limit(values):
    """The values rounded to a thousandth, or None when any is one the viewer would refuse."""
    rounded = [_round_thousandth(value) for value in values]
    if all(math.isfinite(value) and abs(value) <= MAX_OVERLAY_COORDINATE for value in rounded):
        return rounded
    return None


def _overlay_text(overlay) -> str | None:
    """One overlay as the header carries it, or None for one the viewer would refuse (checked after rounding)."""

    def number(value):
        return _number_text(value, MAX_OVERLAY_COORDINATE)

    if isinstance(overlay, Polygon):
        if not 3 <= len(overlay.points) <= MAX_POLYGON_POINTS:
            return None
        points = []
        for point in overlay.points:
            xy = _rounded_within_limit(point)
            if xy is None:
                return None
            points.append(f"[{number(xy[0])},{number(xy[1])}]")
        text = '{"shape":"polygon","points":[' + ",".join(points) + "]"
    elif isinstance(overlay, Rectangle):
        values = _rounded_within_limit((overlay.x, overlay.y, overlay.width, overlay.height))
        if values is None or values[2] <= 0.0 or values[3] <= 0.0:
            return None
        x, y, width, height = (number(value) for value in values)
        text = f'{{"shape":"rectangle","x":{x},"y":{y},"width":{width},"height":{height}'
    elif isinstance(overlay, Circle):
        values = _rounded_within_limit((overlay.x, overlay.y, overlay.radius))
        if values is None or values[2] <= 0.0:
            return None
        x, y, radius = (number(value) for value in values)
        text = f'{{"shape":"circle","x":{x},"y":{y},"radius":{radius}'
    else:
        values = _rounded_within_limit((overlay.x, overlay.y))
        if values is None:
            return None
        x, y = (number(value) for value in values)
        text = f'{{"shape":"point","x":{x},"y":{y}'
    if overlay.label:
        text += ',"label":' + _json_string(_cut_utf8(_valid_text(overlay.label), MAX_LABEL_BYTES))
    colour = overlay.colour.upper()
    if colour != OVERLAY_GREEN:
        text += f',"colour":"{colour}"'
    return text + "}"


# --- The image -------------------------------------------------------------------------------------------------------

# By the kind and size of an array's elements: the depth it goes out as, and the size of each element once converted.
_DEPTHS = {
    ("u", 1): ("8U", 1),
    ("i", 1): ("8S", 1),
    ("u", 2): ("16U", 2),
    ("i", 2): ("16S", 2),
    ("i", 4): ("32S", 4),
    ("f", 4): ("32F", 4),
    ("f", 8): ("64F", 8),
    ("b", 1): ("8U", 1),  # True as 255
    ("f", 2): ("32F", 4),  # half precision, widened
}
_TRANSFERS = ("sdr", "hlg", "pq")
_PRIMARIES = ("bt709", "bt2020")


def _check_image(image) -> tuple[int, int, int, str]:
    """rows, cols, channels and depth of an image the viewer can take, from its metadata alone; ValueError otherwise."""
    array = np.asarray(image)
    if array.ndim == 2:
        rows, cols, channels = array.shape[0], array.shape[1], 1
    elif array.ndim == 3:
        rows, cols, channels = array.shape
    else:
        raise ValueError(f"unsupported image: {array.ndim} dimensions, expected 2 or 3")
    if not (1 <= rows <= MAX_DIMENSION and 1 <= cols <= MAX_DIMENSION and 1 <= channels <= MAX_CHANNELS):
        raise ValueError(f"unsupported image: shape {array.shape}")
    depth = _DEPTHS.get((array.dtype.kind, array.dtype.itemsize))
    if depth is None:
        raise ValueError(
            f"unsupported image: dtype {array.dtype}; convert it, for example with astype(np.int32)"
        )
    if rows * cols * channels * depth[1] > MAX_PIXEL_BYTES:
        raise ValueError("unsupported image: more than 512 MiB of pixels")
    return rows, cols, channels, depth[0]


def _wire_pixels(image, rgb: bool) -> np.ndarray:
    """The pixels as the wire carries them: little-endian, tightly packed, BGR(A) order; the caller's array untouched."""
    array = np.asarray(image)
    if array.dtype.kind == "b":
        array = array.astype(np.uint8) * np.uint8(255)
    elif array.dtype.kind == "f" and array.dtype.itemsize == 2:
        array = array.astype(np.float32)
    if array.dtype.str.startswith(">"):
        array = array.astype(array.dtype.newbyteorder("<"))
    if rgb and array.ndim == 3 and array.shape[2] in (3, 4):
        array = array[..., [2, 1, 0, 3][: array.shape[2]]]
    return np.ascontiguousarray(array)


def _check_description(value_range, transfer, primaries, overlays) -> None:
    """Arguments of the wrong kind are a programming error: raised at once, before anything is sent."""
    if value_range is not None:
        if (
            isinstance(value_range, (str, bytes))
            or not hasattr(value_range, "__len__")
            or len(value_range) != 2
        ):
            raise TypeError("value_range is a pair of numbers, (min, max)")
        if not all(isinstance(end, (numbers.Real, np.number)) for end in value_range):
            raise TypeError("value_range is a pair of numbers, (min, max)")
    if transfer is not None and transfer not in _TRANSFERS:
        raise ValueError(f"transfer is one of {_TRANSFERS}, not {transfer!r}")
    if primaries is not None and primaries not in _PRIMARIES:
        raise ValueError(f"primaries is one of {_PRIMARIES}, not {primaries!r}")
    if overlays is not None and not all(isinstance(overlay, _SHAPES) for overlay in overlays):
        raise TypeError("overlays are Polygon, Rectangle, Circle or Point")


def _header(
    rows, cols, channels, depth, name, live, sent_at, value_range, transfer, primaries, overlays
) -> bytes:
    fields = [
        '"protocol":"visual_debug"',
        f'"version":{PROTOCOL_VERSION}',
        '"type":"image"',
        '"name":' + _json_string(_cut_utf8(_valid_text(name), MAX_NAME_BYTES)),
        f'"rows":{rows}',
        f'"cols":{cols}',
        f'"depth":"{depth}"',
        f'"channels":{channels}',
    ]
    if live:
        fields.append('"live":true')
    if sent_at is not None and 0 <= int(sent_at) <= MAX_SENT_AT_MILLISECONDS:
        fields.append(f'"sentAt":{int(sent_at)}')
    if value_range is not None:
        low, high = float(value_range[0]), float(value_range[1])
        if math.isfinite(low) and math.isfinite(high) and low < high:
            low_text = _number_text(low, _LARGEST_EXACT_INTEGER)
            high_text = _number_text(high, _LARGEST_EXACT_INTEGER)
            fields.append(f'"valueRange":[{low_text},{high_text}]')
    if transfer is not None:
        fields.append(f'"transfer":"{transfer}"')
    if primaries is not None:
        fields.append(f'"primaries":"{primaries}"')
    header = "{" + ",".join(fields)
    if overlays:
        items = []
        for overlay in overlays:
            if len(items) == MAX_OVERLAYS:
                break
            text = _overlay_text(overlay)
            if text is not None:
                items.append(text)
        # As many as fit 64 KiB, from the first, measured as C++ measures them: in UTF-8 bytes, with their commas.
        size = len((header + "}").encode()) + len(',"overlays":[]')
        fit = 0
        for item in items:
            item_size = len(item.encode()) + (1 if fit > 0 else 0)
            if size + item_size > MAX_HEADER_BYTES:
                break
            size += item_size
            fit += 1
        if fit > 0:
            header += ',"overlays":[' + ",".join(items[:fit]) + "]"
    return (header + "}").encode()


def encode(
    image,
    name: str,
    *,
    live: bool = False,
    sent_at: int | None = None,
    rgb: bool = False,
    value_range=None,
    transfer: str | None = None,
    primaries: str | None = None,
    overlays=None,
) -> tuple[bytes, memoryview]:
    """The two frames of a request: the JSON header, and the pixels as a byte view of a contiguous little-endian copy
    (or of the caller's own array when it already is one). ValueError for an image the viewer cannot take; TypeError or
    ValueError for arguments of the wrong kind."""
    overlays = None if overlays is None else list(overlays)  # a generator is read once
    _check_description(value_range, transfer, primaries, overlays)
    rows, cols, channels, depth = _check_image(image)
    header = _header(
        rows,
        cols,
        channels,
        depth,
        name,
        live,
        sent_at,
        value_range,
        transfer,
        primaries,
        list(overlays or []),
    )
    if len(header) > MAX_HEADER_BYTES:
        raise ValueError("header too large")
    return header, memoryview(_wire_pixels(image, rgb)).cast("B")


# --- Sending (libs/visual_debug/src/client.cpp, step for step) -------------------------------------------------------


class SendResult(enum.Enum):
    """What became of an image; str() gives the C++ toString() text."""

    DELIVERED = "Delivered"
    VIEWER_UNAVAILABLE = "Viewer unavailable"
    REJECTED = "Rejected"
    INVALID_IMAGE = "Invalid image"
    TIMEOUT = "Timeout"
    INVALID_ENDPOINT = "Invalid endpoint"

    def __str__(self) -> str:
        return self.value


def default_endpoint() -> str:
    """VISUAL_DEBUG_ENDPOINT when it is set and not empty, else tcp://127.0.0.1:7139. The only place that reads it."""
    return os.environ.get("VISUAL_DEBUG_ENDPOINT") or DEFAULT_ENDPOINT


# Whatever answers on the viewer's port cannot push a large reply into the host application.
_MAX_REPLY_BYTES = 64 * 1024
_MEBIBYTE = 1024 * 1024

# The module's own context, never zmq.Context.instance(), which a Jupyter kernel uses itself. A forked child cannot use
# its parent's, so it makes its own.
_context = None
_context_pid = None
_context_lock = threading.Lock()


def _shared_context() -> zmq.Context:
    global _context, _context_pid
    with _context_lock:
        if _context is None or _context_pid != os.getpid():
            _context = zmq.Context()
            _context_pid = os.getpid()
        return _context


def _wait_for(socket, event: int, timeout: float) -> bool:
    """True when the socket is ready for `event` within `timeout` seconds."""
    return bool(socket.poll(max(0, round(timeout * 1000)), event) & event)


def _is_ok_reply(reply: bytes) -> bool:
    try:
        value = json.loads(reply)
    except Exception:  # not UTF-8, not JSON, or nested too deeply: in any case not "ok"
        return False
    return isinstance(value, dict) and value.get("status") == "ok"


class Client:
    """Sends images to a viewer over ZeroMQ REQ/REP. With no viewer running, the host program is blocked once for the
    reply timeout, not on every send; once a viewer appears, sending recovers by itself. Times are in seconds.
    Not thread-safe, as the C++ Client: use one per thread, or send_image(), which serialises its calls."""

    def __init__(self, endpoint: str | None = None, reply_timeout: float = 0.5, retry_interval: float = 1.0):
        self.endpoint = default_endpoint() if endpoint is None else endpoint
        self.reply_timeout = float(reply_timeout)
        self.retry_interval = float(retry_interval)
        self._socket = None
        self._socket_pid = None
        self._socket_is_fresh = False
        self._back_off_until = 0.0

    def send(
        self, image, name: str, *, rgb=False, value_range=None, transfer=None, primaries=None, overlays=None
    ) -> SendResult:
        """Sends an image to the viewer's history. TypeError or ValueError only for arguments of the wrong kind."""
        return self._send(image, name, False, rgb, value_range, transfer, primaries, overlays)

    def send_live(
        self,
        frame,
        channel: str,
        *,
        rgb=False,
        value_range=None,
        transfer=None,
        primaries=None,
        overlays=None,
    ) -> SendResult:
        """Sends one frame of a live channel: the viewer shows it in place of the channel's previous frame."""
        return self._send(frame, channel, True, rgb, value_range, transfer, primaries, overlays)

    def close(self) -> None:
        self._drop_socket()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _send(self, image, name, live, rgb, value_range, transfer, primaries, overlays) -> SendResult:
        overlays = None if overlays is None else list(overlays)
        _check_description(value_range, transfer, primaries, overlays)
        # Cheap checks first: while no viewer is available, an image is never converted or copied.
        try:
            _check_image(image)
        except ValueError:
            return SendResult.INVALID_IMAGE
        if time.monotonic() < self._back_off_until:
            return SendResult.VIEWER_UNAVAILABLE
        if self._socket is not None and self._socket_pid != os.getpid():
            self._socket = None  # the parent's, after a fork: unusable here, and not ours to close
        if self._socket is None and not self._open_socket():
            return SendResult.INVALID_ENDPOINT

        # Only a fresh socket waits for its connection. Afterwards ZeroMQ keeps reconnecting in the background, so a
        # program without a viewer is blocked once, not on every retry.
        connect_wait = self.reply_timeout if self._socket_is_fresh else 0.0
        self._socket_is_fresh = False
        if not _wait_for(self._socket, zmq.POLLOUT, connect_wait):
            self._start_back_off()
            return SendResult.VIEWER_UNAVAILABLE

        sent_at = time.time_ns() // 1_000_000 if live else None
        try:
            header, pixels = encode(
                image,
                name,
                live=live,
                sent_at=sent_at,
                rgb=rgb,
                value_range=value_range,
                transfer=transfer,
                primaries=primaries,
                overlays=overlays,
            )
        except ValueError:
            return SendResult.INVALID_IMAGE

        # A REQ socket interrupted between its request and the reply could never send again: drop it, so the next
        # send starts afresh, then let the interrupt through.
        try:
            try:
                # copy=True: the caller may change the array as soon as send returns.
                self._socket.send_multipart([header, pixels], flags=zmq.NOBLOCK, copy=True)
            except zmq.ZMQError:
                self._recover()
                return SendResult.VIEWER_UNAVAILABLE
            try:
                if not _wait_for(self._socket, zmq.POLLIN, self._reply_deadline(pixels.nbytes)):
                    self._recover()  # a lost reply would block the REQ socket for good ("Lazy Pirate")
                    return SendResult.TIMEOUT
                reply = self._socket.recv(flags=zmq.NOBLOCK)
            except zmq.ZMQError:
                self._recover()
                return SendResult.TIMEOUT
        except KeyboardInterrupt:
            self._drop_socket()
            raise
        return SendResult.DELIVERED if _is_ok_reply(reply) else SendResult.REJECTED

    def _open_socket(self) -> bool:
        socket = _shared_context().socket(zmq.REQ)
        socket.setsockopt(zmq.LINGER, 0)  # the program's exit never waits for undelivered images
        socket.setsockopt(zmq.IMMEDIATE, 1)  # no connection means "unavailable", not "queued"
        socket.setsockopt(zmq.MAXMSGSIZE, _MAX_REPLY_BYTES)
        socket.setsockopt(zmq.IPV6, 1)  # dual-stack: tcp://[::1] works as well as 127.0.0.1
        # Abandoning a connection attempt after the reply timeout lets ZeroMQ retry, so a viewer that starts later is
        # found as quickly on Windows, which takes about 2 s to report a refused connection, as elsewhere.
        socket.setsockopt(zmq.CONNECT_TIMEOUT, min(max(0, round(self.reply_timeout * 1000)), 2**31 - 1))
        try:
            socket.connect(self.endpoint)
        except zmq.ZMQError:
            socket.close(linger=0)
            return False
        self._socket = socket
        self._socket_pid = os.getpid()
        self._socket_is_fresh = True
        return True

    def _reply_deadline(self, pixel_bytes: int) -> float:
        """Copying a large image through loopback takes a while, so each started MiB adds 10 ms."""
        return self.reply_timeout + 0.010 * ((pixel_bytes + _MEBIBYTE - 1) // _MEBIBYTE)

    def _recover(self) -> None:
        """After an unexpected error the socket's state is unknown: drop it and back off."""
        self._drop_socket()
        self._start_back_off()

    def _drop_socket(self) -> None:
        if self._socket is not None and self._socket_pid == os.getpid():
            self._socket.close(linger=0)
        self._socket = None

    def _start_back_off(self) -> None:
        self._back_off_until = time.monotonic() + self.retry_interval


# One client for the whole process, made on first use (reading the endpoint then), behind a lock.
_shared_client = None
_shared_client_lock = threading.Lock()


def send_image(image, name: str, **description) -> SendResult:
    """The one-line debugging call: sends an image to the viewer at default_endpoint() through one process-wide Client.
    Safe from any thread. Keywords as Client.send: rgb, value_range, transfer, primaries, overlays."""
    global _shared_client
    with _shared_client_lock:
        if _shared_client is None:
            _shared_client = Client()
        return _shared_client.send(image, name, **description)


def send_live(frame, channel: str, **description) -> SendResult:
    """The same for one frame of a live channel, such as a camera. Shares send_image's client."""
    global _shared_client
    with _shared_client_lock:
        if _shared_client is None:
            _shared_client = Client()
        return _shared_client.send_live(frame, channel, **description)


def _reset_shared_client() -> None:
    """Closes the shared client, so the next send makes a new one and reads the endpoint again. For tests."""
    global _shared_client
    with _shared_client_lock:
        if _shared_client is not None:
            _shared_client.close()
        _shared_client = None
