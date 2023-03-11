import subprocess
import tempfile
import soundfile as sf
import os, shutil
import uuid

from tts import get_audio_from_text

def cache_file(key):
    cache_dir = "cache"
    if not os.path.exists(cache_dir):
        os.makedirs(cache_dir)
    return os.path.join(cache_dir, str(uuid.uuid4()))

def search_and_download_audio(key):
    name = cache_file(key) + ".wav"
    fmt = "wav"
    cmd = f'yt-dlp --force-overwrite -f "ba" -x --audio-format {fmt} ytsearch1:"{key}" -o {name}'
    subprocess.check_call(cmd, shell=True)
    return name

def save_audio(audio, samp_rate):
    tmpFile = tempfile.NamedTemporaryFile(suffix='.wav')
    sf.write(tmpFile, audio, samp_rate, format="WAV")
    tmpFile.seek(0)
    return tmpFile

def perform_task(intent):
    cmd = intent["command"]
    args = intent["arguments"]
    phrase = " ".join(args)

    if cmd == "play":
        return search_and_download_audio(phrase)
    elif cmd == "repeat":
        return save_audio(*get_audio_from_text(phrase))
    else:
        return save_audio(*get_audio_from_text("sorry, could you please repeat?"))
