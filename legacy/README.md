# legacy/

The original Python implementation of hoodie, kept for reference only. It is no
longer built or run; the C++ port lives in `../src/`.

| File | What it was |
|---|---|
| `main.py` | Entry point: wake → record → ASR → agent → TTS loop |
| `audio.py` | PortAudio capture/playback (`sounddevice`), WAV I/O (`soundfile`) |
| `wakeword.py` | Wake-word detection (`openwakeword`) |
| `vad.py` | Voice activity detection (`webrtcvad`) |
| `agent.py` | `pydantic_ai` tool-calling agent |
| `tools.py` | Tools: time, weather, music (`yt-dlp`) |
| `actions.py` | ASR/LLM/TTS HTTP calls (`requests`) |
| `google_pse.py` | Unused Google Programmable Search helper |
| `config.py` | Configuration |
| `logger.py` | Logging setup |
| `requirements.txt` | Python dependencies |
| `TODO.md` | Original backlog |

Notes:

- **Security:** `config.py` contains a hard-coded API key that is present in git
  history. Treat it as compromised and rotate it.
- Do not copy its defects into new work - see `../ARCHITECTURE.md` section 15
  (inverted VAD stop, dead failure path, naive wake-word decimation, `shell=True`,
  blocking requests).
- `beep.ogg` intentionally stayed at the repository root because the C++ tests
  and `--selftest` reference it.
