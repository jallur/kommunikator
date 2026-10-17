"""In-memory conversation ids for xAI session resumption.

History expires after 30 minutes of inactivity, matching the docs.
The key is a hash of the device token so the token is not stored.
"""

from __future__ import annotations

import hashlib
import time
from collections.abc import Callable


class ConversationMemory:
    def __init__(
        self,
        ttl_s: float = 30 * 60,
        clock: Callable[[], float] | None = None,
    ) -> None:
        self.ttl_s = ttl_s
        self._clock = clock or time.monotonic
        self._items: dict[str, tuple[str, float]] = {}

    def get(self, token: str) -> str | None:
        key = _key(token)
        item = self._items.get(key)
        if item is None:
            return None
        conversation_id, seen = item
        if self._clock() - seen > self.ttl_s:
            self._items.pop(key, None)
            return None
        return conversation_id

    def put(self, token: str, conversation_id: str) -> None:
        if not conversation_id:
            return
        self._items[_key(token)] = (conversation_id, self._clock())


def _key(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()
