"""A tour of the Python sender. Start the Visual Debug Viewer, then run

    python python/examples/demo.py              # to VISUAL_DEBUG_ENDPOINT, or tcp://127.0.0.1:7139
    python python/examples/demo.py --endpoint tcp://127.0.0.1:7140

It sends a gradient, a 10-bit image with its value range, a card with overlays, and a short live sequence. It needs
only numpy and pyzmq, and finds visual_debug.py beside its own folder, so nothing has to be installed.
"""

import argparse
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import visual_debug as vd  # noqa: E402  (after the path, so the demo runs without an install)

LIVE_FRAMES = 30


def gradient() -> np.ndarray:
    """8-bit BGR, as OpenCV holds images: blue grows to the right, red fades downwards."""
    image = np.zeros((360, 640, 3), np.uint8)
    image[..., 0] = np.linspace(0, 255, 640)[None, :]
    image[..., 2] = np.linspace(255, 0, 360)[:, None]
    return image


def ten_bit_ramp() -> np.ndarray:
    """Values 0 to 1023 in a 16-bit array: without its value range the viewer would show it almost black."""
    return (np.add.outer(np.arange(480), np.arange(640)) % 1024).astype(np.uint16)


def card() -> tuple[np.ndarray, list]:
    image = np.full((400, 640, 3), 40, np.uint8)
    image[60:340, 80:560] = (200, 190, 180)
    image[100:220, 420:540] = (120, 140, 160)
    overlays = [
        vd.Polygon([(80, 60), (560, 60), (560, 340), (80, 340)], label="card"),
        vd.Rectangle(100, 260, 300, 50, label="MRZ", colour=vd.OVERLAY_RED),
        vd.Circle(480, 160, 70, label="photo", colour=vd.OVERLAY_AMBER),
        vd.Point(320, 200, label="centre"),
    ]
    return image, overlays


def live_frame(index: int) -> np.ndarray:
    """A bright bar sweeping across a dark grey frame."""
    frame = np.full((240, 320), 30, np.uint8)
    column = (index * 10) % 300
    frame[:, column : column + 20] = 230
    return frame


def main() -> int:
    parser = argparse.ArgumentParser(description="Send a few example images to the Visual Debug Viewer.")
    parser.add_argument(
        "--endpoint", help="the viewer's endpoint (default: VISUAL_DEBUG_ENDPOINT or port 7139)"
    )
    endpoint = parser.parse_args().endpoint

    results = {}
    with vd.Client(endpoint) as client:
        print(f"Sending to {client.endpoint}")
        results["gradient"] = client.send(gradient(), "demo: gradient")
        results["10-bit ramp"] = client.send(ten_bit_ramp(), "demo: 10-bit ramp", value_range=(0, 1023))
        image, overlays = card()
        results["overlays"] = client.send(image, "demo: card with overlays", overlays=overlays)
        # Live frames replace one another in one channel instead of filling the history: a camera, a video.
        for index in range(LIVE_FRAMES):
            result = client.send_live(live_frame(index), "demo: live")
            if result is not vd.SendResult.DELIVERED:
                break
            time.sleep(1 / LIVE_FRAMES)
        results[f"{LIVE_FRAMES} live frames"] = result
    for what, result in results.items():
        print(f"{what}: {result}")
    return 0 if all(result is vd.SendResult.DELIVERED for result in results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
