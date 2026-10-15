from app.resample import Pcm16Resampler


def _pcm(samples: list[int]) -> bytes:
    out = bytearray()
    for sample in samples:
        out += int(sample).to_bytes(2, "little", signed=True)
    return bytes(out)


def _samples(pcm: bytes) -> list[int]:
    return [
        int.from_bytes(pcm[i : i + 2], "little", signed=True) for i in range(0, len(pcm), 2)
    ]


def test_equal_rate_is_identity() -> None:
    src = _pcm([0, 1, -2, 32767, -32768])
    assert Pcm16Resampler(16000, 16000).process(src) == src


def test_odd_trailing_byte_is_dropped() -> None:
    assert Pcm16Resampler(16000, 16000).process(b"\x01\x00\xff") == b"\x01\x00"


def test_silence_and_dc_survive_rate_change() -> None:
    silence = _pcm([0] * 16)
    assert Pcm16Resampler(16000, 24000).process(silence)
    dc = _pcm([1000] * 32)
    changed = _samples(Pcm16Resampler(16000, 8000).process(dc))
    assert changed
    assert set(changed) == {1000}


def test_chunked_dc_matches_one_buffer() -> None:
    samples = [1000] * 40
    whole = _samples(Pcm16Resampler(24000, 16000).process(_pcm(samples)))
    left = Pcm16Resampler(24000, 16000)
    chunked = _samples(left.process(_pcm(samples[:15]))) + _samples(
        left.process(_pcm(samples[15:]))
    )
    assert set(whole) == {1000}
    assert set(chunked) == {1000}
    assert abs(len(whole) - len(chunked)) <= 2


def test_empty_input() -> None:
    assert Pcm16Resampler(16000, 24000).process(b"") == b""
