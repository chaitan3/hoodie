#!/usr/bin/env python3
"""Minimal OpenAI-compatible mock server for integration testing hoodie.

Serves the three buffered endpoints the C++ client uses. Test-only; not part of
the application.

  POST /chat/completions            -> {"choices":[{"message":{...}}]}
  POST /v1/audio/transcriptions     -> {"text": "..."}
  POST /v1/audio/speech             -> PCM (response_format=pcm) or WAV tone
  GET  /health                      -> {"status":"ok"}
"""
import argparse
import json
import math
import struct
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SAMPLE_RATE = 24000
TONE_SECONDS = 0.4
REPLY_TEXT = "mock reply: pong"
FINAL_TEXT = "mock final reply"
TRANSCRIPT_TEXT = "mock transcript"


def tone_pcm() -> bytes:
    count = int(SAMPLE_RATE * TONE_SECONDS)
    samples = [
        int(math.sin(2.0 * math.pi * 440.0 * i / SAMPLE_RATE) * 12000.0) for i in range(count)
    ]
    return struct.pack("<%dh" % count, *samples)


def pcm_to_wav(pcm: bytes) -> bytes:
    data_size = len(pcm)
    header = b"RIFF" + struct.pack("<I", 36 + data_size) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, SAMPLE_RATE, SAMPLE_RATE * 2, 2, 16)
    header += b"data" + struct.pack("<I", data_size)
    return header + pcm


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quieter test output
        pass

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", "0") or "0")
        return self.rfile.read(length) if length else b""

    def _send_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_bytes(self, status: int, content_type: str, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_chunked(self, content_type: str, chunks) -> None:
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for chunk in chunks:
            if not chunk:
                continue
            self.wfile.write(f"{len(chunk):X}\r\n".encode() + chunk + b"\r\n")
            self.wfile.flush()
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def _send_sse(self, events) -> None:
        payloads = [("data: " + json.dumps(event) + "\n\n").encode() for event in events]
        payloads.append(b"data: [DONE]\n\n")
        self._send_chunked("text/event-stream", payloads)

    def _send_llm_stream(self, request) -> None:
        tools = request.get("tools") or []
        messages = request.get("messages") or []
        used_tool = any(m.get("role") == "tool" for m in messages)
        events = []
        if tools and not used_tool:
            events.append(
                {
                    "choices": [
                        {
                            "delta": {
                                "role": "assistant",
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "id": "call_1",
                                        "type": "function",
                                        "function": {"name": "get_time", "arguments": ""},
                                    }
                                ],
                            }
                        }
                    ]
                }
            )
            events.append(
                {"choices": [{"delta": {"tool_calls": [{"index": 0, "function": {"arguments": "{}"}}]}}]}
            )
            events.append({"choices": [{"delta": {}, "finish_reason": "tool_calls"}]})
        else:
            text = FINAL_TEXT if (tools and used_tool) else REPLY_TEXT
            for i in range(0, len(text), 8):
                events.append({"choices": [{"delta": {"content": text[i : i + 8]}}]})
            events.append({"choices": [{"delta": {}, "finish_reason": "stop"}]})
        self._send_sse(events)

    def do_GET(self):  # noqa: N802
        if self.path == "/health":
            self._send_json(200, {"status": "ok"})
        else:
            self._send_json(404, {"error": {"message": "not found"}})

    def do_POST(self):  # noqa: N802
        body = self._read_body()
        if self.path.endswith("/chat/completions"):
            try:
                request = json.loads(body or b"{}")
            except json.JSONDecodeError:
                request = {}
            tools = request.get("tools") or []
            messages = request.get("messages") or []
            used_tool = any(m.get("role") == "tool" for m in messages)
            if request.get("stream"):
                self._send_llm_stream(request)
            elif tools and not used_tool:
                # Scripted first round: ask the client to call get_time.
                self._send_json(
                    200,
                    {
                        "choices": [
                            {
                                "message": {
                                    "role": "assistant",
                                    "content": None,
                                    "tool_calls": [
                                        {
                                            "id": "call_1",
                                            "type": "function",
                                            "function": {"name": "get_time", "arguments": "{}"},
                                        }
                                    ],
                                },
                                "finish_reason": "tool_calls",
                            }
                        ]
                    },
                )
            elif tools and used_tool:
                self._send_json(
                    200,
                    {
                        "choices": [
                            {
                                "message": {"role": "assistant", "content": FINAL_TEXT},
                                "finish_reason": "stop",
                            }
                        ]
                    },
                )
            else:
                self._send_json(
                    200,
                    {
                        "choices": [
                            {
                                "message": {"role": "assistant", "content": REPLY_TEXT},
                                "finish_reason": "stop",
                            }
                        ],
                        "usage": {"prompt_tokens": 3, "completion_tokens": 2},
                    },
                )
        elif self.path.endswith("/audio/transcriptions"):
            self._send_json(200, {"text": TRANSCRIPT_TEXT})
        elif self.path.endswith("/audio/speech"):
            try:
                request = json.loads(body or b"{}")
            except json.JSONDecodeError:
                request = {}
            pcm = tone_pcm()
            if request.get("stream_format") == "audio" and request.get("response_format") == "pcm":
                third = max(1, len(pcm) // 3)
                self._send_chunked(
                    "audio/pcm", [pcm[i : i + third] for i in range(0, len(pcm), third)]
                )
            elif request.get("response_format") == "pcm":
                self._send_bytes(200, "audio/pcm", pcm)
            else:
                self._send_bytes(200, "audio/wav", pcm_to_wav(pcm))
        else:
            self._send_json(404, {"error": {"message": "not found"}})


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8099)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"mock openai server on http://{args.host}:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
