import subprocess
import os, shutil, glob
import uuid
from threading import Thread
import time
import json

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

import requests
from config import api_key, base_url
def get_text_from_audio(audio_file):
    headers = {"Authorization": f"Bearer {api_key}"}
    response = requests.post(f"{base_url}/v1/audio/transcriptions",
                             headers = headers,
                             json={
                                 'file': audio_file,
                                 'model': "small"
                            })
    response = response.json()
    text = response["text"]
    return text

def get_audio_from_text(text):
    headers = {"Authorization": f"Bearer {api_key}"}
    response = requests.post(f"{base_url}/v1/audio/speech",
                             headers = headers,
                             json={
                                 "input": text,
                                 "model": "tts-1"
                            })
    audio = response.content
    return audio

def search_music(phrase):
    cmd = f'yt-dlp --dump-json ytsearch1:"{phrase}"'
    output = subprocess.check_output(cmd, shell=True)
    config = json.loads(output.decode('utf-8'))
    return config

def download_music(key, config):
    name = os.path.splitext(key)[0]
    cmd = f'yt-dlp --force-overwrite -f "ba" -x --audio-format vorbis {config["id"]} -o {name}'
    subprocess.check_call(cmd, shell=True)

def perform_task(intent, player):
    cmd = intent["command"]
    args = intent["arguments"]
    phrase = " ".join(args)

    if cmd == "play" and len(phrase) > 0:
        config = search_music(phrase)
        out_phrase = f'Playing {config["title"]}'
        # download_music(config)
    elif cmd == "repeat" and len(phrase) > 0:
        out_phrase = phrase
    elif (cmd == "stop") or (cmd == "false_positive"):
        return
    else:
        out_phrase = "Sorry, could you please repeat?"
    audio_file = BytesIO(get_audio_from_text(out_phrase))
    player.queue(audio_file)

import struct
import numpy as np
import soundfile as sf
def save_audio(audio_data):
    with open("out.wav", "wb") as f:
        num_samples = len(audio_data) // 2  # assume 16-bit PCM
        waveform = np.array(struct.unpack(str(num_samples) + "H", audio_data)).astype(np.int16)
        sf.write(f, waveform, 16000)
        #sf.write(f, audio, 16000, format="OGG", subtype="VORBIS")

if __name__ == "__main__":
    audio = get_audio_from_text("hello")
