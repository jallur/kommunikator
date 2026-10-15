"""Per-device bearer tokens. Compared in constant time, never logged."""

from __future__ import annotations

import hmac


def token_allowed(presented: str, allowed: tuple[str, ...]) -> bool:
    if not presented or not allowed:
        return False
    ok = False
    presented_b = presented.encode("utf-8")
    for token in allowed:
        ok |= hmac.compare_digest(presented_b, token.encode("utf-8"))
    return ok


def bearer_from_header(value: str | None) -> str:
    if not value:
        return ""
    prefix = "bearer "
    if value.lower().startswith(prefix):
        return value[len(prefix) :].strip()
    return ""
