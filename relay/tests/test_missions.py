from fastapi.testclient import TestClient

from app.main import create_app


def test_mission_route_is_a_todo(settings) -> None:
    client = TestClient(create_app(settings))
    denied = client.post("/v1/missions", json={"device_id": "stick", "text": "ring hem"})
    assert denied.status_code == 401

    accepted_shape = client.post(
        "/v1/missions",
        headers={"Authorization": "Bearer device-token-a"},
        json={"device_id": "stick", "text": "ring hem"},
    )
    assert accepted_shape.status_code == 501
    assert "TODO" in accepted_shape.json()["detail"]

    invalid = client.post(
        "/v1/missions",
        headers={"Authorization": "Bearer device-token-a"},
        json={"device_id": "", "text": ""},
    )
    assert invalid.status_code == 422
