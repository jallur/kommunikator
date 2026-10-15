from fastapi.testclient import TestClient

from app.main import create_app
from app.settings import Settings


def test_health_does_not_need_a_token() -> None:
    app = create_app(Settings(xai_api_key="", device_tokens=()))
    client = TestClient(app)
    for path in ("/health", "/healthz"):
        response = client.get(path)
        assert response.status_code == 200
        assert response.json() == {"status": "ok"}
