"""Sending, as the C++ client sends: against a fake viewer on a loopback port of each test's own."""

import time

import numpy as np
import pytest
import zmq
from fake_viewer import FakeViewer, free_port

import visual_debug as vd

DELIVERED = vd.SendResult.DELIVERED
UNAVAILABLE = vd.SendResult.VIEWER_UNAVAILABLE
REPLY_TIMEOUT = 0.2
RETRY_INTERVAL = 0.3


def client_for(endpoint):
    return vd.Client(endpoint, reply_timeout=REPLY_TIMEOUT, retry_interval=RETRY_INTERVAL)


def small():
    return np.zeros((2, 3), np.uint8)


def test_delivered_and_received_as_sent():
    image = np.arange(12, dtype=np.uint16).reshape(3, 4)
    with FakeViewer() as fake, client_for(fake.endpoint) as client:
        assert client.send(image, "ramp", value_range=(0, 11)) is DELIVERED
    header, pixels = fake.received[0]
    assert (header["name"], header["depth"], header["valueRange"]) == ("ramp", "16U", [0, 11])
    assert "live" not in header and "sentAt" not in header
    assert pixels == image.tobytes()


def test_live_is_marked_and_stamped():
    with FakeViewer() as fake, client_for(fake.endpoint) as client:
        before = time.time() * 1000
        assert client.send_live(small(), "camera") is DELIVERED
    header = fake.received[0][0]
    assert header["live"] is True
    assert abs(header["sentAt"] - before) < 5000


def test_rejected():
    with FakeViewer(reply="error") as fake, client_for(fake.endpoint) as client:
        assert client.send(small(), "x") is vd.SendResult.REJECTED
        fake.reply = "garbage"
        assert client.send(small(), "x") is vd.SendResult.REJECTED


def test_invalid_image_before_any_network():
    with FakeViewer() as fake, client_for(fake.endpoint) as client:
        assert client.send(np.zeros((2, 2), np.int64), "x") is vd.SendResult.INVALID_IMAGE
        assert client.send(np.zeros((2, 2, 5), np.uint8), "x") is vd.SendResult.INVALID_IMAGE
    assert fake.received == []
    # The image is checked before the back-off: an unsupported one says so even while no viewer is there.
    with client_for(f"tcp://127.0.0.1:{free_port()}") as client:
        assert client.send(small(), "x") is UNAVAILABLE
        assert client.send(np.zeros((2, 2), np.int64), "x") is vd.SendResult.INVALID_IMAGE


def test_invalid_endpoint():
    with vd.Client("nonsense://x") as client:
        assert client.send(small(), "x") is vd.SendResult.INVALID_ENDPOINT
        assert client.send(small(), "x") is vd.SendResult.INVALID_ENDPOINT  # no back-off


def test_wrong_kinds_raise_before_anything_is_sent():
    with client_for(f"tcp://127.0.0.1:{free_port()}") as client:
        start = time.monotonic()
        with pytest.raises(ValueError):
            client.send(small(), "x", transfer="hdr")
        with pytest.raises(TypeError):
            client.send(small(), "x", overlays=[(1, 2)])
        assert time.monotonic() - start < 0.1
    with pytest.raises(ValueError):
        vd.send_image(small(), "x", primaries="p3")


def test_no_viewer_waits_once_then_backs_off():
    with client_for(f"tcp://127.0.0.1:{free_port()}") as client:
        start = time.monotonic()
        assert client.send(small(), "x") is UNAVAILABLE
        assert time.monotonic() - start >= REPLY_TIMEOUT * 0.75
        start = time.monotonic()
        assert client.send(small(), "x") is UNAVAILABLE
        assert time.monotonic() - start < 0.05


def test_timeout_then_recovery():
    with FakeViewer(reply="silent") as fake, client_for(fake.endpoint) as client:
        start = time.monotonic()
        assert client.send(small(), "x") is vd.SendResult.TIMEOUT
        assert REPLY_TIMEOUT * 0.75 <= time.monotonic() - start < REPLY_TIMEOUT + 1.0
        assert client.send(small(), "x") is UNAVAILABLE  # within the back-off
        fake.reply = "ok"
        time.sleep(RETRY_INTERVAL + 0.05)
        assert client.send(small(), "x") is DELIVERED  # on a new socket


def test_viewer_appearing_later_is_found():
    endpoint = f"tcp://127.0.0.1:{free_port()}"
    with client_for(endpoint) as client:
        assert client.send(small(), "x") is UNAVAILABLE
        with FakeViewer(endpoint=endpoint):
            deadline = time.monotonic() + 2.0
            result = client.send(small(), "x")
            while result is not DELIVERED and time.monotonic() < deadline:
                time.sleep(0.05)
                result = client.send(small(), "x")
            assert result is DELIVERED


def test_array_can_change_after_send():
    image = np.full((2, 3), 7, np.uint8)
    with FakeViewer() as fake, client_for(fake.endpoint) as client:
        assert client.send(image, "x") is DELIVERED
        image[:] = 9
    assert fake.received[0][1] == bytes([7] * 6)


def test_interrupt_drops_the_socket(monkeypatch):
    real_wait = vd._wait_for
    interrupted = []

    def wait_interrupted_once(socket, event, timeout):
        if event == zmq.POLLIN and not interrupted:
            interrupted.append(True)
            raise KeyboardInterrupt
        return real_wait(socket, event, timeout)

    monkeypatch.setattr(vd, "_wait_for", wait_interrupted_once)
    with FakeViewer() as fake, client_for(fake.endpoint) as client:
        with pytest.raises(KeyboardInterrupt):
            client.send(small(), "x")
        # A REQ socket still owed a reply could not send again: the interrupted one was dropped.
        assert client.send(small(), "x") is DELIVERED


def test_shared_client_reads_the_endpoint_on_first_use(isolated_endpoint, monkeypatch):
    with FakeViewer(endpoint=isolated_endpoint) as fake:
        assert vd.send_image(small(), "shared") is DELIVERED
    assert fake.received[0][0]["name"] == "shared"
    second = f"tcp://127.0.0.1:{free_port()}"
    monkeypatch.setenv("VISUAL_DEBUG_ENDPOINT", second)
    vd._reset_shared_client()
    with FakeViewer(endpoint=second) as fake:
        assert vd.send_live(small(), "camera") is DELIVERED
    assert fake.received[0][0]["live"] is True


def test_default_endpoint(monkeypatch):
    assert vd.default_endpoint() != "tcp://127.0.0.1:7139"  # the fixture's port, never the owner's viewer
    monkeypatch.setenv("VISUAL_DEBUG_ENDPOINT", "")
    assert vd.default_endpoint() == "tcp://127.0.0.1:7139"
    monkeypatch.delenv("VISUAL_DEBUG_ENDPOINT")
    assert vd.default_endpoint() == "tcp://127.0.0.1:7139"


def test_str_of_results():
    assert str(vd.SendResult.VIEWER_UNAVAILABLE) == "Viewer unavailable"
    assert [str(result) for result in vd.SendResult] == [
        "Delivered",
        "Viewer unavailable",
        "Rejected",
        "Invalid image",
        "Timeout",
        "Invalid endpoint",
    ]
