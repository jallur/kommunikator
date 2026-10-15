"""Interface for a later Grok Bot webhook. No outbound call is made."""

from __future__ import annotations

from pydantic import BaseModel, Field


class MissionNotConfigured(Exception):
    pass


class Mission(BaseModel):
    device_id: str = Field(min_length=1, max_length=64)
    text: str = Field(min_length=1, max_length=4000)
    mission_id: str | None = None


class GrokBotWebhookDispatcher:
    """TODO: CoreIT has not published a Grok Bot webhook URL or payload.

    `url` is stored so the setting has a place to land. dispatch() does not
    POST anywhere, even if the variable is set.
    """

    def __init__(self, url: str | None) -> None:
        self.url = url

    async def dispatch(self, mission: Mission) -> None:
        del mission
        raise MissionNotConfigured(
            "TODO: Grok Bot-webhook är inte kopplad. Ingen adress anropas."
        )
