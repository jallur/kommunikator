import pytest

from app.settings import Settings


@pytest.fixture
def settings() -> Settings:
    return Settings(
        xai_api_key="xai-test-key",
        device_tokens=("device-token-a", "device-token-b"),
    )
