"""Local speech-to-text and Ollama bridge for the ESP32 voice bot."""

from __future__ import annotations

import json
import re
import sys
import tempfile
import threading
import urllib.error
import urllib.request
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from faster_whisper import WhisperModel


HOST = "0.0.0.0"
PORT = 8000
OLLAMA_CHAT_URL = "http://127.0.0.1:11434/api/chat"
OLLAMA_SPEECH_URL = "http://127.0.0.1:11434/v1/audio/speech"
OLLAMA_CHAT_MODEL = "qwen3:8b"
OLLAMA_SPEECH_MODEL = "lfm3-audio:latest"
MAX_WAV_BYTES = 1_000_000
INTERRUPT_EVENT = threading.Event()
REQUEST_ACTIVE = threading.Event()

print("Loading Whisper base.en on CPU (first run may download the model)...", flush=True)
whisper = WhisperModel("base.en", device="cpu", compute_type="int8")
print(
    f"Whisper ready. Ollama chat model: {OLLAMA_CHAT_MODEL}; "
    f"speech model: {OLLAMA_SPEECH_MODEL}",
    flush=True,
)


def json_response(handler: BaseHTTPRequestHandler, status: int, payload: dict) -> None:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.send_header("Access-Control-Allow-Origin", "*")
    handler.end_headers()
    handler.wfile.write(body)


def transcribe_wav(audio: bytes) -> str:
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as temp_file:
        temp_file.write(audio)
        wav_path = Path(temp_file.name)
    try:
        with wave.open(str(wav_path), "rb") as wav_file:
            if wav_file.getnchannels() != 1 or wav_file.getframerate() != 16000:
                raise ValueError("Expected 16 kHz mono WAV from the ESP32.")
        segments, _info = whisper.transcribe(
            str(wav_path), language="en", beam_size=3, vad_filter=True
        )
        return " ".join(segment.text.strip() for segment in segments).strip()
    finally:
        wav_path.unlink(missing_ok=True)


def answer_with_ollama(transcript: str) -> str:
    request_data = {
        "model": OLLAMA_CHAT_MODEL,
        "stream": False,
        "think": False,
        "messages": [
            {
                "role": "system",
                "content": (
                    "You are a helpful voice assistant. Answer directly in one short, "
                    "natural sentence. Do not add a story or an introduction. If the "
                    "question is unclear, ask the user to repeat it."
                ),
            },
            {"role": "user", "content": transcript},
        ],
        "options": {"temperature": 0.2, "num_predict": 80},
    }
    req = urllib.request.Request(
        OLLAMA_CHAT_URL,
        data=json.dumps(request_data).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=180) as response:
        result = json.loads(response.read().decode("utf-8"))
    answer = result.get("message", {}).get("content", "").strip()
    if not answer:
        raise RuntimeError(
            f"Ollama returned no answer text for {OLLAMA_CHAT_MODEL}: {result}"
        )
    return answer


class VoiceBridgeHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self) -> None:
        if self.path == "/speak":
            self.handle_speech()
            return
        if self.path == "/ask":
            self.handle_streaming_ask()
            return
        json_response(self, 404, {"error": "Use POST /ask or /speak."})

    def handle_streaming_ask(self) -> None:
        """Stream successive answer segments as PCM until the client or user stops."""
        active_upstream = None
        response_started = False
        INTERRUPT_EVENT.clear()
        REQUEST_ACTIVE.set()
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 44 or length > MAX_WAV_BYTES:
                json_response(self, 413, {"error": "Invalid WAV size."})
                return
            audio = self.rfile.read(length)
            if len(audio) != length or audio[:4] != b"RIFF" or audio[8:12] != b"WAVE":
                json_response(self, 400, {"error": "Expected a WAV recording."})
                return

            # Start the response before transcription so the ESP32 can poll its
            # touch input while Whisper and Ollama are working.
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("X-Audio-Format", "s16le;rate=24000;channels=1")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            response_started = True

            def send_pcm(data: bytes) -> None:
                if not data or INTERRUPT_EVENT.is_set():
                    return
                self.wfile.write(f"{len(data):X}\r\n".encode("ascii"))
                self.wfile.write(data)
                self.wfile.write(b"\r\n")
                self.wfile.flush()

            def speak_segment(text: str) -> None:
                if not text.strip() or INTERRUPT_EVENT.is_set():
                    return
                payload = {
                    "model": OLLAMA_SPEECH_MODEL,
                    "input": text.strip(),
                    "response_format": "pcm",
                    "stream_format": "audio",
                }
                req = urllib.request.Request(
                    OLLAMA_SPEECH_URL,
                    data=json.dumps(payload).encode("utf-8"),
                    headers={"Content-Type": "application/json"},
                    method="POST",
                )
                speech_response = urllib.request.urlopen(req, timeout=180)
                pending = b""
                try:
                    while not INTERRUPT_EVENT.is_set():
                        data = speech_response.read1(2048)
                        if not data:
                            break
                        data = pending + data
                        even_length = len(data) & ~1
                        if even_length:
                            send_pcm(data[:even_length])
                        pending = data[even_length:]
                finally:
                    speech_response.close()

            transcript = transcribe_wav(audio)
            print(f"Transcript: {transcript or '[empty]'}", flush=True)
            if not transcript:
                speak_segment("I could not hear that. Please repeat.")
            else:
                messages = [
                    {
                        "role": "system",
                        "content": (
                            "Answer helpfully and in detail. Speak naturally. Keep expanding "
                            "with relevant information; do not conclude because the listener "
                            "will interrupt when they want to stop."
                        ),
                    },
                    {"role": "user", "content": transcript},
                ]
                while not INTERRUPT_EVENT.is_set():
                    payload = {
                        "model": OLLAMA_CHAT_MODEL,
                        "stream": True,
                        "think": False,
                        "messages": messages,
                        "options": {"temperature": 0.3, "num_predict": 1024},
                    }
                    req = urllib.request.Request(
                        OLLAMA_CHAT_URL,
                        data=json.dumps(payload).encode("utf-8"),
                        headers={"Content-Type": "application/json"},
                        method="POST",
                    )
                    active_upstream = urllib.request.urlopen(req, timeout=300)
                    buffered_text = ""
                    turn_text = ""
                    try:
                        for line in active_upstream:
                            if INTERRUPT_EVENT.is_set():
                                break
                            if not line.strip():
                                continue
                            item = json.loads(line.decode("utf-8"))
                            token = item.get("message", {}).get("content", "")
                            if token:
                                buffered_text += token
                                turn_text += token
                            while buffered_text:
                                match = re.search(r"[.!?][\"')\]]?\s", buffered_text)
                                split_at = match.end() if match else 0
                                if not split_at and len(buffered_text) >= 240:
                                    split_at = buffered_text.rfind(" ", 0, 220)
                                if not split_at:
                                    break
                                speak_segment(buffered_text[:split_at])
                                buffered_text = buffered_text[split_at:]
                                if INTERRUPT_EVENT.is_set():
                                    break
                            if item.get("done"):
                                break
                    finally:
                        active_upstream.close()
                        active_upstream = None
                    if INTERRUPT_EVENT.is_set() or not turn_text.strip():
                        break
                    speak_segment(buffered_text)
                    messages = [
                        messages[0],
                        {"role": "user", "content": transcript},
                        {"role": "assistant", "content": turn_text[-6000:]},
                        {
                            "role": "user",
                            "content": "Continue with more relevant detail. Do not repeat or conclude.",
                        },
                    ]
        except (BrokenPipeError, ConnectionResetError):
            INTERRUPT_EVENT.set()
            print("ESP32 disconnected; stopping the stream.", flush=True)
        except Exception as exc:
            print(f"Streaming request failed: {exc}", flush=True)
        finally:
            if active_upstream is not None:
                active_upstream.close()
            if response_started:
                try:
                    trailer = b"X-Interrupted: 1\r\n" if INTERRUPT_EVENT.is_set() else b""
                    self.wfile.write(b"0\r\n" + trailer + b"\r\n")
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
            REQUEST_ACTIVE.clear()

    def handle_speech(self) -> None:
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 12000:
                json_response(self, 413, {"error": "Invalid speech request size."})
                return
            request_data = json.loads(self.rfile.read(length).decode("utf-8"))
            text = str(request_data.get("text", "")).strip()
            if not text or len(text) > 4096:
                json_response(self, 400, {"error": "Provide text of 1 to 4096 characters."})
                return

            speech_request = {
                "model": OLLAMA_SPEECH_MODEL,
                "input": text,
                "response_format": "pcm",
                "stream_format": "audio",
            }
            req = urllib.request.Request(
                OLLAMA_SPEECH_URL,
                data=json.dumps(speech_request).encode("utf-8"),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=180) as response:
                pcm = response.read()
            if not pcm or len(pcm) % 2:
                json_response(self, 502, {"error": "Ollama returned invalid PCM audio."})
                return

            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("X-Audio-Format", "s16le;rate=24000;channels=1")
            self.send_header("Content-Length", str(len(pcm)))
            self.end_headers()
            self.wfile.write(pcm)
            print(f"Ollama speech generated: {len(pcm)} PCM bytes", flush=True)
        except urllib.error.URLError as exc:
            print(f"Ollama speech request failed: {exc}", flush=True)
            json_response(self, 502, {"error": "Could not generate speech with local Ollama."})
        except Exception as exc:
            print(f"Speech request failed: {exc}", flush=True)
            json_response(self, 500, {"error": str(exc)})

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"{self.client_address[0]} - {fmt % args}", flush=True)


if __name__ == "__main__":
    def keyboard_interrupt_listener() -> None:
        print("During streaming playback, type r and press Enter to interrupt.", flush=True)
        while True:
            command = sys.stdin.readline()
            if not command:
                return
            if command.strip().lower() in {"r", "stop"} and REQUEST_ACTIVE.is_set():
                INTERRUPT_EVENT.set()
                print("Interrupt received; returning to the next listen cycle.", flush=True)

    threading.Thread(target=keyboard_interrupt_listener, daemon=True).start()
    server = ThreadingHTTPServer((HOST, PORT), VoiceBridgeHandler)
    print(f"Voice bridge listening on http://{HOST}:{PORT}/ask and /speak", flush=True)
    print("Keep Ollama running; press Ctrl+C to stop this bridge.", flush=True)
    server.serve_forever()
