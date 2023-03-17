import subprocess
import os, shutil, glob
import soundfile as sf
import uuid
from threading import Thread
import time
import json

from tts import get_audio_from_text

sleep_time = 10 # ms
audio_mimetype = "audio/ogg"

class ActionsRunner(Thread):
    def __init__(self):
        Thread.__init__(self)
        self.tasks = {}
        self.active = True
        self.cache = set()

    def get_cache(self, key=None):
        if key is None:
            key = str(uuid.uuid4())
        cache_dir = os.path.join(os.getcwd(), "cache")
        if not os.path.exists(cache_dir):
            os.makedirs(cache_dir)
        return os.path.join(cache_dir, key)

    def queue(self, func, args):
        key = self.get_cache() + ".ogg"
        if key not in self.tasks:
            self.tasks[key] = (func, args)
        return os.path.basename(key)

    def stop(self):
        self.active = False

    def run(self):
        while self.active:
            if len(self.tasks) > 0:
                key, (func, args) = next(iter(self.tasks.items()))
                func(key, *args)
                self.cache.add(key)
                del self.tasks[key]
            time.sleep(sleep_time / 1000)

runner = ActionsRunner()
runner.daemon = True

def search_music(phrase):
    cmd = f'yt-dlp --dump-json ytsearch1:"{phrase}"'
    output = subprocess.check_output(cmd, shell=True)
    config = json.loads(output.decode('utf-8'))
    return config

def download_music(key, config):
    name = os.path.splitext(key)[0]
    cmd = f'yt-dlp --force-overwrite -f "ba" -x --audio-format vorbis {config["id"]} -o {name}'
    subprocess.check_call(cmd, shell=True)

def save_audio_from_text(key, phrase):
    audio, samp_rate = get_audio_from_text(phrase)
    sf.write(key, audio, samp_rate, format="OGG", subtype="VORBIS")

def perform_task(intent):
    cmd = intent["command"]
    args = intent["arguments"]
    phrase = " ".join(args)

    if cmd == "play" and len(phrase) > 0:
        config = search_music(phrase)
        return [runner.queue(save_audio_from_text, (f'Playing {config["title"]}',)),
                runner.queue(download_music, (config,))]
    elif cmd == "repeat" and len(phrase) > 0:
        return [runner.queue(save_audio_from_text, (phrase,))]
    elif cmd == "stop":
        return []
    else:
        phrase = "Sorry, could you please repeat?"
        return [runner.queue(save_audio_from_text, (phrase,))]
