import pytest

from app.tokens import bearer_from_header, token_allowed


def test_rejects_missing_and_unknown_tokens() -> None:
    allowed = ("alpha-token", "beta-token")
    assert token_allowed("", allowed) is False
    assert token_allowed("nope", allowed) is False
    assert token_allowed("alpha-token", ()) is False


def test_accepts_each_configured_token() -> None:
    allowed = ("alpha-token", "beta-token")
    assert token_allowed("alpha-token", allowed) is True
    assert token_allowed("beta-token", allowed) is True


def test_different_length_does_not_raise() -> None:
    assert token_allowed("short", ("a-much-longer-token",)) is False


def test_bearer_header() -> None:
    assert bearer_from_header("Bearer device-token") == "device-token"
    assert bearer_from_header("bearer device-token") == "device-token"
    assert bearer_from_header(None) == ""
    assert bearer_from_header("Basic abc") == ""


def test_bad_rate_is_rejected() -> None:
    from app.settings import Settings

    with pytest.raises(ValueError, match="PCM rate"):
        Settings(xai_api_key="k", device_tokens=("t",), xai_input_rate=11025)
    with pytest.raises(ValueError, match="wss://api.x.ai/v1/realtime"):
        Settings(
            xai_api_key="k",
            device_tokens=("t",),
            xai_realtime_url="wss://example.invalid/v1/realtime",
        )
    with pytest.raises(ValueError, match="deprecated"):
        Settings(xai_api_key="k", device_tokens=("t",), xai_model="grok-voice-2.0")
