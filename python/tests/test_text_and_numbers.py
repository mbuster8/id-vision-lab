"""The wire format's number and text rules, as the C++ sender applies them (libs/visual_debug/src/wire_format.cpp)."""

import math

import numpy as np

import visual_debug as vd


def test_number_layout_matches_nlohmann():
    # nlohmann's format_buffer: fixed while the point is 1 to 15 places in, 0.000ddd down to 4 zeros, else d.ddde±XX.
    assert vd._format_number(0.5) == "0.5"
    assert vd._format_number(123.456) == "123.456"
    assert vd._format_number(0.0001) == "0.0001"
    assert vd._format_number(0.00012) == "0.00012"
    assert vd._format_number(1e-05) == "1e-05"
    assert vd._format_number(1.5e-05) == "1.5e-05"
    assert vd._format_number(123456789012345.5) == "123456789012345.5"
    assert vd._format_number(1e15 + 0.5) == "1.0000000000000005e+15"
    assert vd._format_number(9007199254740994.0) == "9.007199254740994e+15"
    assert vd._format_number(1e16) == "1e+16"
    assert vd._format_number(-2.5) == "-2.5"
    assert vd._format_number(1.25e300) == "1.25e+300"
    assert vd._format_number(float(np.float32(0.1))) == "0.10000000149011612"


def test_rounding_halves_away_from_zero():
    assert vd._round_thousandth(0.0125) == 0.013
    assert vd._round_thousandth(-0.0125) == -0.013
    assert vd._round_thousandth(0.0005) == 0.001
    assert vd._round_thousandth(0.0004) == 0.0
    assert vd._round_thousandth(1.0005) == 1.001
    assert vd._round_thousandth(np.float32(0.1)) == 0.1  # widened to a double before multiplying, as in C++


def test_rounding_of_what_cannot_be_rounded_is_not_finite():
    assert math.isnan(vd._round_thousandth(float("nan")))
    assert math.isinf(vd._round_thousandth(float("inf")))
    assert math.isinf(vd._round_thousandth(1.8e306))  # ×1000 overflows: left for the limit check to drop


def test_whole_numbers_are_integers():
    assert vd._whole_or_float(1023.0, 2.0**53) == 1023
    assert isinstance(vd._whole_or_float(1023.0, 2.0**53), int)
    assert vd._whole_or_float(-0.0, 1e6) == 0
    assert isinstance(vd._whole_or_float(-0.0, 1e6), int)
    assert isinstance(vd._whole_or_float(2.0**53 + 2, 2.0**53), float)
    assert vd._whole_or_float(0.25, 1e6) == 0.25


def test_lone_surrogates_become_one_replacement_each():
    assert vd._valid_text("a\ud800b") == "a�b"
    assert vd._valid_text("\udfff\ud800") == "��"
    assert vd._valid_text("Sobel X é 😀") == "Sobel X é 😀"


def test_cut_at_a_character_boundary():
    assert vd._cut_utf8("é" * 600, 1024) == "é" * 512
    assert vd._cut_utf8("a" + "é" * 600, 1024) == "a" + "é" * 511
    assert vd._cut_utf8("😀" * 300, 1024) == "😀" * 256
    assert vd._cut_utf8("short", 1024) == "short"


def test_json_string_escapes_as_nlohmann():
    assert vd._json_string('a"b\\c\n\x01é') == '"a\\"b\\\\c\\n\\u0001é"'
    assert vd._json_string("tab\tform\fback\bcr\r") == '"tab\\tform\\fback\\bcr\\r"'
    assert vd._json_string("/\x7f ") == '"/\x7f "'
