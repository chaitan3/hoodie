import subprocess
import tempfile
import os, shutil, glob
import soundfile as sf
import uuid

from tts import get_audio_from_text

def cache_file(key):
    cache_dir = os.path.join(os.getcwd(), "cache")
    if not os.path.exists(cache_dir):
        os.makedirs(cache_dir)
    return os.path.join(cache_dir, str(uuid.uuid4()))

def search_and_download_audio(key):
    name = cache_file(key)
    cmd = f'yt-dlp --force-overwrite -f "ba" -x ytsearch1:"{key}" -o "{name}.%(ext)s"'
    subprocess.check_call(cmd, shell=True)
    file_name = glob.glob(name + ".*")[0]
    return file_name, "audio/ogg"

def save_audio_from_text(phrase):
    audio, samp_rate = get_audio_from_text(phrase)
    tmpFile = tempfile.NamedTemporaryFile(suffix='.ogg')
    sf.write(tmpFile, audio, samp_rate, format="OGG", subtype="VORBIS")
    tmpFile.seek(0)
    return tmpFile, "audio/ogg"

def perform_task(intent):
    cmd = intent["command"]
    args = intent["arguments"]
    phrase = " ".join(args)

    if cmd == "play" and len(phrase) > 0:
        return search_and_download_audio(phrase)
    elif cmd == "repeat" and len(phrase) > 0:
        return save_audio_from_text(phrase)
    else:
        return save_audio_from_text("sorry, could you please repeat?")
