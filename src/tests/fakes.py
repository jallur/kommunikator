import asyncio
import json


class FakeLink:
    """xAI stand-in. `mode` scripts the races the relay has to ignore.

    auto: one commit, one transcript, one reply.
    partial: an updated transcript and no completed event.
    race: the first commit's transcript arrives after the second commit.
    late_done: the first response's done arrives after the next response exists.
    """

    def __init__(
        self,
        downlink_pcm: bytes = b"\x00\x00" * 24,
        vad_reply: bool = False,
        mode: str = "auto",
    ) -> None:
        self.sent: list[str | bytes] = []
        self.incoming: asyncio.Queue[str | bytes | None] = asyncio.Queue()
        self.downlink_pcm = downlink_pcm
        self.vad_reply = vad_reply
        self.mode = mode
        self._commits = 0
        self._creates = 0
        self._loop: asyncio.AbstractEventLoop | None = None

    def push(self, event: dict) -> None:
        """Enqueue from the test thread. The app loop owns the queue."""
        payload = json.dumps(event)
        loop = self._loop
        if loop is None:
            self.incoming.put_nowait(payload)
            return
        loop.call_soon_threadsafe(self.incoming.put_nowait, payload)

    async def send(self, message: str | bytes) -> None:
        self._loop = asyncio.get_running_loop()
        self.sent.append(message)
        if not isinstance(message, str):
            return
        event = json.loads(message)
        kind = event.get("type")
        if kind == "session.update":
            await self._put({"type": "session.updated", "session": event["session"]})
        elif kind == "input_audio_buffer.commit":
            await self._on_commit()
        elif kind == "response.create":
            await self._on_create()

    async def _on_commit(self) -> None:
        self._commits += 1
        if self.mode == "race":
            if self._commits == 1:
                return
            await self._put({"type": "input_audio_buffer.committed", "item_id": "item_a"})
            await self._put(
                {
                    "type": "conversation.item.input_audio_transcription.completed",
                    "item_id": "item_a",
                    "transcript": "första frågan",
                }
            )
            await self._put({"type": "input_audio_buffer.committed", "item_id": "item_b"})
            await self._put(
                {
                    "type": "conversation.item.input_audio_transcription.completed",
                    "item_id": "item_b",
                    "transcript": "andra frågan",
                }
            )
            return
        item_id = f"item_{self._commits}"
        if self.mode == "partial":
            # Updated events are documented as arriving while the person speaks,
            # which can be before the committed event for that same item.
            await self._put(
                {
                    "type": "conversation.item.input_audio_transcription.updated",
                    "item_id": item_id,
                    "transcript": "delvis fråga",
                }
            )
        await self._put({"type": "input_audio_buffer.committed", "item_id": item_id})
        if self.vad_reply:
            # Real speech makes the live API open the response itself,
            # before our response.create, even when turn_detection is null.
            await self._put({"type": "response.created", "response": {"id": f"resp_{self._commits}"}})
            await self._put_bytes(self.downlink_pcm)
        if self.mode == "partial":
            return
        await self._put(
            {
                "type": "conversation.item.input_audio_transcription.completed",
                "item_id": item_id,
                "transcript": "Vad är 5 plus 5?",
            }
        )
        if self.vad_reply:
            await self._put(
                {
                    "type": "response.output_audio_transcript.delta",
                    "response_id": f"resp_{self._commits}",
                    "delta": "Hej",
                }
            )
            await self._put(
                {"type": "response.done", "response": {"id": f"resp_{self._commits}"}}
            )

    async def _on_create(self) -> None:
        if self.vad_reply:
            return
        self._creates += 1
        response_id = f"resp_{self._creates}"
        await self._put({"type": "response.created", "response": {"id": response_id}})
        await self._put_bytes(self.downlink_pcm)
        await self._put(
            {
                "type": "response.output_audio_transcript.delta",
                "response_id": response_id,
                "delta": "Hej",
            }
        )
        if self.mode == "late_done" and self._creates == 1:
            return
        if self.mode == "late_done" and self._creates == 2:
            await self._put({"type": "response.done", "response": {"id": "resp_1"}})
            return
        await self._put({"type": "response.done", "response": {"id": response_id}})

    async def _put(self, event: dict) -> None:
        await self.incoming.put(json.dumps(event))

    async def _put_bytes(self, payload: bytes) -> None:
        await self.incoming.put(payload)

    async def recv(self) -> str | bytes:
        item = await self.incoming.get()
        if item is None:
            raise ConnectionError("closed")
        return item

    async def close(self) -> None:
        await self.incoming.put(None)


class FakeFactory:
    def __init__(
        self,
        downlink_pcm: bytes = b"\x00\x00" * 24,
        vad_reply: bool = False,
        mode: str = "auto",
    ) -> None:
        self.links: list[FakeLink] = []
        self.urls: list[str] = []
        self.keys: list[str] = []
        self.downlink_pcm = downlink_pcm
        self.vad_reply = vad_reply
        self.mode = mode

    async def connect(self, url: str, api_key: str) -> FakeLink:
        self.urls.append(url)
        self.keys.append(api_key)
        link = FakeLink(self.downlink_pcm, vad_reply=self.vad_reply, mode=self.mode)
        self.links.append(link)
        await link.incoming.put(json.dumps({"type": "session.created"}))
        await link.incoming.put(
            json.dumps({"type": "conversation.created", "conversation": {"id": "conv_test"}})
        )
        return link
