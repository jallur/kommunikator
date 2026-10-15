"""Streaming PCM16 little-endian resampler.

Linear interpolation between samples. One source sample of latency is held
so chunk boundaries do not click. Equal rates are a straight copy.
"""

from __future__ import annotations


def _decode(pcm: bytes) -> list[int]:
    if len(pcm) % 2:
        pcm = pcm[:-1]
    return [
        int.from_bytes(pcm[i : i + 2], "little", signed=True) for i in range(0, len(pcm), 2)
    ]


def _encode(samples: list[int]) -> bytes:
    out = bytearray()
    for sample in samples:
        if sample > 32767:
            sample = 32767
        elif sample < -32768:
            sample = -32768
        out += int(sample).to_bytes(2, "little", signed=True)
    return bytes(out)


class Pcm16Resampler:
    def __init__(self, src_rate: int, dst_rate: int) -> None:
        if src_rate <= 0 or dst_rate <= 0:
            raise ValueError("sample rate must be positive")
        self.src_rate = src_rate
        self.dst_rate = dst_rate
        self._prev: int | None = None
        self._src_index = 0
        self._out_index = 0

    def process(self, pcm: bytes) -> bytes:
        incoming = _decode(pcm)
        if not incoming:
            return b""
        if self.src_rate == self.dst_rate:
            return _encode(incoming)

        produced: list[int] = []
        if self._prev is None:
            self._prev = incoming[0]
            incoming = incoming[1:]
            self._src_index = 1
            produced.append(self._prev)
            self._out_index = 1

        base = self._src_index
        window = incoming
        last = base + len(window) - 1 if window else base - 1

        def at(index: int) -> int:
            if index == base - 1:
                assert self._prev is not None
                return self._prev
            return window[index - base]

        while self._prev is not None:
            numerator = self._out_index * self.src_rate
            ipos = numerator // self.dst_rate
            if ipos >= last or ipos < base - 1:
                break
            frac = numerator % self.dst_rate
            left = at(ipos)
            right = at(ipos + 1)
            mixed = left + (right - left) * frac / self.dst_rate
            produced.append(int(round(mixed)))
            self._out_index += 1

        if window:
            self._prev = window[-1]
            self._src_index = last + 1
        return _encode(produced)
