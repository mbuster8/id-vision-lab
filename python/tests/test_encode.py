"""encode(): the image, the header and overlays, as the C++ encode() writes them (docs/wire-format.md)."""

import json

import numpy as np
import pytest

import visual_debug as vd

SOBEL_X_HEADER = (
    b'{"protocol":"visual_debug","version":2,"type":"image","name":"Sobel X","rows":480,"cols":640,'
    b'"depth":"16S","channels":1}'
)


def header_of(image, name="x", **options):
    return json.loads(vd.encode(image, name, **options)[0])


def overlays_of(*shapes):
    return header_of(np.zeros((4, 4), np.uint8), overlays=list(shapes)).get("overlays", [])


def test_minimal_header_is_the_wire_formats_example():
    header, pixels = vd.encode(np.zeros((480, 640), np.int16), "Sobel X")
    assert header == SOBEL_X_HEADER
    assert len(header) == 120
    assert pixels.nbytes == 640 * 480 * 2


@pytest.mark.parametrize(
    "shape, channels", [((4, 5), 1), ((4, 5, 1), 1), ((4, 5, 2), 2), ((4, 5, 3), 3), ((4, 5, 4), 4)]
)
def test_shapes(shape, channels):
    header = header_of(np.zeros(shape, np.uint8))
    assert (header["rows"], header["cols"], header["channels"]) == (4, 5, channels)


@pytest.mark.parametrize("shape", [(4, 5, 5), (4,), (2, 2, 2, 2), (0, 5), (5, 0)])
def test_unsupported_shapes(shape):
    with pytest.raises(ValueError):
        vd.encode(np.zeros(shape, np.uint8), "x")


@pytest.mark.parametrize(
    "dtype, depth",
    [
        (np.uint8, "8U"),
        (np.int8, "8S"),
        (np.uint16, "16U"),
        (np.int16, "16S"),
        (np.int32, "32S"),
        (np.float32, "32F"),
        (np.float64, "64F"),
    ],
)
def test_dtypes(dtype, depth):
    image = np.arange(6, dtype=dtype).reshape(2, 3)
    header, pixels = vd.encode(image, "x")
    assert json.loads(header)["depth"] == depth
    assert bytes(pixels) == image.astype(image.dtype.newbyteorder("<")).tobytes()


def test_bool_is_eight_bit_with_255_for_true():
    header, pixels = vd.encode(np.array([[True, False]]), "mask")
    assert json.loads(header)["depth"] == "8U"
    assert bytes(pixels) == bytes([255, 0])


def test_half_precision_is_sent_as_float32():
    image = np.array([[0.5, -2.0]], np.float16)
    header, pixels = vd.encode(image, "x")
    assert json.loads(header)["depth"] == "32F"
    assert bytes(pixels) == np.array([[0.5, -2.0]], "<f4").tobytes()


@pytest.mark.parametrize("dtype", [np.int64, np.uint32, np.uint64, np.complex64, object])
def test_unsupported_dtypes(dtype):
    with pytest.raises(ValueError):
        vd.encode(np.zeros((2, 2), dtype), "x")


def test_byte_order_and_contiguity():
    big = np.arange(6, dtype=">u2").reshape(2, 3)
    assert bytes(vd.encode(big, "x")[1]) == big.astype("<u2").tobytes()
    base = np.arange(24, dtype=np.uint8).reshape(4, 6)
    assert bytes(vd.encode(base[:, ::2], "x")[1]) == np.ascontiguousarray(base[:, ::2]).tobytes()
    assert bytes(vd.encode(base.T, "x")[1]) == np.ascontiguousarray(base.T).tobytes()
    assert base[0, 1] == 1  # the caller's array is untouched


def test_rgb_swaps_red_and_blue():
    three = np.array([[[1, 2, 3]]], np.uint8)
    four = np.array([[[1, 2, 3, 4]]], np.uint8)
    grey = np.array([[7]], np.uint8)
    assert bytes(vd.encode(three, "x", rgb=True)[1]) == bytes([3, 2, 1])
    assert bytes(vd.encode(four, "x", rgb=True)[1]) == bytes([3, 2, 1, 4])
    assert bytes(vd.encode(grey, "x", rgb=True)[1]) == bytes([7])
    assert bytes(vd.encode(three, "x")[1]) == bytes([1, 2, 3])


def test_size_limits_are_checked_on_metadata_alone():
    one = np.zeros(1, np.uint8)
    tall = np.lib.stride_tricks.as_strided(one, shape=(32769, 1), strides=(0, 0))
    with pytest.raises(ValueError):
        vd.encode(tall, "x")
    # 512 MiB as float16, 1 GiB once sent as float32: refused before anything is allocated.
    half = np.zeros(1, np.float16)
    huge = np.lib.stride_tricks.as_strided(half, shape=(16384, 16384), strides=(0, 0))
    with pytest.raises(ValueError):
        vd.encode(huge, "x")


def test_name_rules():
    assert header_of(np.zeros((1, 1), np.uint8), "a" * 1100)["name"] == "a" * 1024
    assert header_of(np.zeros((1, 1), np.uint8), "bad \ud800 name")["name"] == "bad � name"


def test_live_and_sent_at():
    header, _ = vd.encode(np.zeros((1, 1), np.uint8), "camera", live=True, sent_at=1793366602517)
    assert header.endswith(b'"channels":1,"live":true,"sentAt":1793366602517}')
    for outside in (-1, 2**60):
        assert "sentAt" not in header_of(np.zeros((1, 1), np.uint8), live=True, sent_at=outside)


def range_text(value_range):
    header, _ = vd.encode(np.zeros((1, 1), np.uint16), "x", value_range=value_range)
    text = header.decode()
    return text[text.index('"valueRange":') + len('"valueRange":') : -1] if "valueRange" in text else None


def test_value_range():
    assert range_text((0, 1023)) == "[0,1023]"
    assert range_text((0.0, 1.0)) == "[0,1]"
    assert range_text((0.25, 0.75)) == "[0.25,0.75]"
    assert range_text((np.float32(0.1), 1)) == "[0.10000000149011612,1]"
    assert range_text((0, 2**53 + 1)) == "[0,9007199254740992]"  # held as a double, as C++ holds it
    assert range_text((0.5, 1e16)) == "[0.5,1e+16]"
    for empty in ((5, 5), (9, 3), (float("nan"), 1), (0, float("inf"))):
        assert range_text(empty) is None


def test_transfer_and_primaries():
    header = header_of(np.zeros((1, 1, 3), np.uint16), transfer="hlg", primaries="bt2020")
    assert (header["transfer"], header["primaries"]) == ("hlg", "bt2020")
    for wrong in ({"transfer": "HLG"}, {"transfer": "hdr"}, {"primaries": "p3"}):
        with pytest.raises(ValueError):
            vd.encode(np.zeros((1, 1), np.uint8), "x", **wrong)


def test_overlay_rounding_and_layout():
    assert overlays_of(vd.Circle(1.0005, 2, 0.0005)) == [
        {"shape": "circle", "x": 1.001, "y": 2, "radius": 0.001}
    ]
    assert overlays_of(vd.Rectangle(1, 2, 3, 4, label="box", colour="#e05050")) == [
        {"shape": "rectangle", "x": 1, "y": 2, "width": 3, "height": 4, "label": "box", "colour": "#E05050"}
    ]
    assert overlays_of(vd.Point(5, 6, colour=vd.OVERLAY_GREEN)) == [{"shape": "point", "x": 5, "y": 6}]
    assert overlays_of(vd.Polygon([(0, 0), (1, 0), (0, 1)], label="")) == [
        {"shape": "polygon", "points": [[0, 0], [1, 0], [0, 1]]}
    ]


def test_overlays_the_viewer_would_refuse_are_left_out():
    left_out = [
        vd.Point(float("nan"), 1),
        vd.Point(1e6 + 1, 1),
        vd.Circle(1, 1, 0.0004),
        vd.Rectangle(1, 1, 0, 5),
        vd.Polygon([(0, 0), (1, 1)]),
        vd.Polygon([(i, i) for i in range(1025)]),
        vd.Point(1.8e306, 0),
    ]
    assert overlays_of(*left_out) == []
    assert "overlays" not in header_of(np.zeros((1, 1), np.uint8), overlays=left_out)


def test_at_most_256_shapes_and_labels_of_64_bytes():
    assert len(overlays_of(*[vd.Point(i, i) for i in range(300)])) == 256
    assert overlays_of(vd.Point(0, 0, label="l" * 70))[0]["label"] == "l" * 64
    assert overlays_of(vd.Point(0, 0, label="a" + "é" * 40))[0]["label"] == "a" + "é" * 31


@pytest.mark.parametrize(
    "points", [np.zeros((4, 3)), [1, 2, 3, 4, 5, 6], np.zeros((2, 2, 2)), [(1, 2, 3)], np.zeros((3, 2, 1))]
)
def test_polygon_points_must_be_pairs(points):
    with pytest.raises(ValueError):
        vd.Polygon(points)


def test_name_must_be_a_string():
    for name in (42, b"bytes", None):
        with pytest.raises(TypeError):
            vd.encode(np.zeros((1, 1), np.uint8), name)


def test_a_contour_is_a_polygon():
    contour = np.array([[[1, 2]], [[3, 4]], [[5, 6]]], np.int32)
    assert vd.Polygon(contour) == vd.Polygon([(1, 2), (3, 4), (5, 6)])
    assert overlays_of(vd.Polygon(contour))[0]["points"] == [[1, 2], [3, 4], [5, 6]]


def test_overlays_are_cut_to_fit_64_kib():
    polygons = [vd.Polygon([(i, i + 0.5) for i in range(1024)]) for _ in range(100)]
    header, _ = vd.encode(np.zeros((1, 1), np.uint8), "x", overlays=polygons)
    kept = json.loads(header)["overlays"]
    assert len(header) <= 65536
    assert 0 < len(kept) < 100
    # The next one would not have fitted: the cut is as late as it can be.
    one_more = len(header) + 1 + len(json.dumps(kept[0], separators=(",", ":")))
    assert one_more > 65536


def test_wrong_kinds_raise():
    with pytest.raises(TypeError):
        vd.encode(np.zeros((1, 1), np.uint8), "x", overlays=[(1, 2)])
    with pytest.raises(ValueError):
        vd.Point(0, 0, colour="red")
    with pytest.raises(ValueError):
        vd.Point(0, 0, colour="#E05050\n")
    with pytest.raises(TypeError):
        vd.encode(np.zeros((1, 1), np.uint8), "x", value_range=5)
