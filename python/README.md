# visual_debug for Python

Sends numpy arrays to the [Visual Debug Viewer](../README.md), exactly as the C++ `visual_debug` library sends
`cv::Mat`s: the same wire format, the same optional fields and overlays, the same results. One module,
`visual_debug.py`, needing only numpy and pyzmq.

```sh
python3 -m venv .venv
.venv/bin/pip install -e python      # from the repository root; or copy visual_debug.py next to your notebook
```

```python
import numpy as np
import visual_debug as vd

print(vd.send_image(np.zeros((480, 640, 3), np.uint8), "black"))   # Delivered, or Viewer unavailable
frame = np.full((240, 320), 128, np.uint8)
print(vd.send_live(frame, "camera", overlays=[vd.Rectangle(10, 20, 100, 50, label="face")]))
```

Arrays are BGR(A), as OpenCV holds them; pass `rgb=True` for RGB(A). The user guide's
[Sending from Python](../docs/user-guide.md#sending-from-python) has the dtypes, the keywords, the results and
the client's options; `examples/demo.py` sends a few images and a short live sequence.

## Versions

It declares Python 3.10, numpy 1.22 and pyzmq 22 or newer. Those floors are declared, not tested: the tests
run on continuous integration with Ubuntu 24.04's own packages (Python 3.12, numpy 1.26, pyzmq 24, pytest 7.4)
and on a Mac with Python 3.15 and numpy 2.5.

## Tests and formatting

```sh
python3 -m venv python/.venv
python/.venv/bin/pip install -e python pytest black==24.2.0
python/.venv/bin/python -m pytest python/tests
python/.venv/bin/black --check python
```

The tests send to a fake viewer on loopback ports of their own, never to a viewer on port 7139.
`test_golden_headers.py` checks that `encode()` writes the headers in `tests/wire/header_cases.json` byte for
byte, as the C++ encoder must too ([wire format](../docs/wire-format.md)).
