import asyncio
import json


class FakeLink:
    def __init__(self, downlink_pcm: bytes = b"\x00\x00" * 24, vad_reply: bool = False) -> None:
        self.sent: list[str | bytes] = []
        self.incoming: asyncio.Queue[str | bytes | None] = asyncio.Queue()
        self.downlink_pcm = downlink_pcm
        self.vad_reply = vad_reply

    async def send(self, message: str | bytes) -> None:
        self.sent.append(message)
        if not isinstance(message, str):
            return
        event = json.loads(message)
        if event.get("type") == "session.update":
            await self.incoming.put(json.dumps({"type": "session.updated", "session": event["session"]}))
        elif event.get("type") == "input_audio_buffer.commit":
            if self.vad_reply:
                # Real speech makes the live API open the response itself,
                # before our response.create, even when turn_detection is null.
                await self.incoming.put(json.dumps({"type": "response.created"}))
                await self.incoming.put(self.downlink_pcm)
            await self.incoming.put(
                json.dumps(
                    {
                        "type": "conversation.item.input_audio_transcription.completed",
                        "transcript": "Vad är 5 plus 5?",
                    }
                )
            )
            if self.vad_reply:
                await self.incoming.put(
                    json.dumps({"type": "response.output_audio_transcript.delta", "delta": "Hej"})
                )
                await self.incoming.put(json.dumps({"type": "response.done"}))
        elif event.get("type") == "response.create":
            if self.vad_reply:
                return
            await self.incoming.put(json.dumps({"type": "response.created"}))
            await self.incoming.put(self.downlink_pcm)
            await self.incoming.put(
                json.dumps({"type": "response.output_audio_transcript.delta", "delta": "Hej"})
            )
            await self.incoming.put(json.dumps({"type": "response.done"}))

    async def recv(self) -> str | bytes:
        item = await self.incoming.get()
        if item is None:
            raise ConnectionError("closed")
        return item

    async def close(self) -> None:
        await self.incoming.put(None)


class FakeFactory:
    def __init__(self, downlink_pcm: bytes = b"\x00\x00" * 24, vad_reply: bool = False) -> None:
        self.links: list[FakeLink] = []
        self.urls: list[str] = []
        self.keys: list[str] = []
        self.downlink_pcm = downlink_pcm
        self.vad_reply = vad_reply

    async def connect(self, url: str, api_key: str) -> FakeLink:
        self.urls.append(url)
        self.keys.append(api_key)
        link = FakeLink(self.downlink_pcm, vad_reply=self.vad_reply)
        self.links.append(link)
        await link.incoming.put(json.dumps({"type": "session.created"}))
        await link.incoming.put(
            json.dumps({"type": "conversation.created", "conversation": {"id": "conv_test"}})
        )
        return link
