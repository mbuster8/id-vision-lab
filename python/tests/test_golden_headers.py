"""The golden headers in tests/wire/header_cases.json, written by the C++ encoder: encode() must write each exactly.

A failure here is Python disagreeing with C++: fix Python, never the file.
"""

import json
from pathlib import Path

import numpy as np
import pytest

import visual_debug as vd

CASES_FILE = Path(__file__).resolve().parents[2] / "tests" / "wire" / "header_cases.json"
CASES = [
    case for case in json.loads(CASES_FILE.read_text(encoding="utf-8"))["cases"] if not case.get("cpp_only")
]

DTYPES = {
    "8U": np.uint8,
    "8S": np.int8,
    "16U": np.uint16,
    "16S": np.int16,
    "32S": np.int32,
    "32F": np.float32,
    "64F": np.float64,
}
SHAPES = {"polygon": vd.Polygon, "rectangle": vd.Rectangle, "circle": vd.Circle, "point": vd.Point}


def number(value):
    """A JSON number, or "nan", "inf" or "-inf"."""
    return float(value) if isinstance(value, str) else value


def overlay(item):
    style = {"label": item.get("label", ""), "colour": item.get("colour", vd.OVERLAY_GREEN)}
    if item["shape"] == "polygon":
        return vd.Polygon([(number(x), number(y)) for x, y in item["points"]], **style)
    fields = {"rectangle": ("x", "y", "width", "height"), "circle": ("x", "y", "radius"), "point": ("x", "y")}
    return SHAPES[item["shape"]](*(number(item[field]) for field in fields[item["shape"]]), **style)


def repeated_polygons(repeat):
    """count polygons of `points` points [i, i + 0.5], generated as the C++ test generates them."""
    return [vd.Polygon([(i, i + 0.5) for i in range(repeat["points"])]) for _ in range(repeat["count"])]


def encoded(case):
    rows, cols, channels = case.get("rows", 1), case.get("cols", 1), case.get("channels", 1)
    shape = (rows, cols) if channels == 1 else (rows, cols, channels)
    image = np.zeros(shape, DTYPES[case.get("depth", "8U")])
    options = {"live": case.get("live", False), "sent_at": case.get("sentAt")}
    if "valueRange" in case:
        options["value_range"] = tuple(number(end) for end in case["valueRange"])
    options["transfer"] = case.get("transfer")
    options["primaries"] = case.get("primaries")
    if "overlays" in case:
        options["overlays"] = [overlay(item) for item in case["overlays"]]
    if "overlaysRepeat" in case:
        options["overlays"] = repeated_polygons(case["overlaysRepeat"])
    return vd.encode(image, case.get("name", "x"), **options)[0]


def test_the_cases_are_there():
    assert len(CASES) > 50
    assert any(case["id"] == "wire-format-sobel-x" for case in CASES)


@pytest.mark.parametrize("case", CASES, ids=[case["id"] for case in CASES])
def test_golden_header(case):
    header = encoded(case)
    if "header" in case:
        assert header.decode("utf-8") == case["header"]
    else:
        kept = len(json.loads(header).get("overlays", []))
        assert (kept, len(header)) == (case["keptShapes"], case["headerBytes"])
