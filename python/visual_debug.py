"""Send numpy images to the Visual Debug Viewer.

A pure-Python sender for the viewer's wire format, version 2 (docs/wire-format.md), written to send exactly what the
C++ visual_debug library sends: for the shared golden cases (tests/wire/header_cases.json) the very same header bytes,
and for any input a header that reads back as the same values.
"""

from __future__ import annotations

import json
import math
import re

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
