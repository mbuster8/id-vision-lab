import pytest
from fake_viewer import free_port

import visual_debug as vd


@pytest.fixture(autouse=True)
def isolated_endpoint(monkeypatch):
    """Every test sends to a loopback port of its own, never to a viewer on 7139, with a fresh shared client."""
    endpoint = f"tcp://127.0.0.1:{free_port()}"
    monkeypatch.setenv("VISUAL_DEBUG_ENDPOINT", endpoint)
    vd._reset_shared_client()
    yield endpoint
    vd._reset_shared_client()
