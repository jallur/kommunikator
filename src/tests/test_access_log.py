import logging
from pathlib import Path

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from app.main import create_app
from app.settings import Settings

SECRET = "device-token-a"


def test_uvicorn_access_log_is_disabled() -> None:
    dockerfile = Path(__file__).resolve().parents[1].joinpath("Dockerfile").read_text()
    assert "--no-access-log" in dockerfile


def test_access_log_is_path_and_status_only(caplog) -> None:
    app = create_app(Settings(xai_api_key="", device_tokens=(SECRET,)))
    client = TestClient(app)
    with caplog.at_level(logging.INFO, logger="kommunikator.access"):
        response = client.get(
            f"/health?token={SECRET}",
            headers={"Authorization": f"Bearer {SECRET}", "Cookie": f"token={SECRET}"},
        )
    assert response.status_code == 200
    records = [record for record in caplog.records if record.name == "kommunikator.access"]
    assert records
    for record in records:
        assert record.args == ("/health", 200)
        message = record.getMessage()
        assert message == "/health 200"
        assert SECRET not in message
        assert "?" not in message
        assert "Bearer" not in message
        assert "Authorization" not in message
        assert "Cookie" not in message


def test_rejected_query_token_is_not_in_the_access_log(caplog) -> None:
    app = create_app(Settings(xai_api_key="", device_tokens=(SECRET,)))
    client = TestClient(app)
    with caplog.at_level(logging.INFO, logger="kommunikator.access"):
        with client.websocket_connect(f"/ws?token={SECRET}") as ws:
            body = ws.receive_json()
            assert body["code"] == "unauthorized"
            with pytest.raises(WebSocketDisconnect):
                ws.receive_json()
    records = [record for record in caplog.records if record.name == "kommunikator.access"]
    assert records
    for record in records:
        message = record.getMessage()
        assert record.args[0] == "/ws"
        assert SECRET not in message
        assert "?" not in message
        assert "token" not in message
