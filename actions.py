import subprocess
import os, shutil, glob
import json
import io
from logger import logging
import re

import requests
from config import api_key, base_url
def get_text_from_audio(audio_file):
    headers = {"Authorization": f"Bearer {api_key}"}
    response = requests.post(f"{base_url}/v1/audio/transcriptions",
                             headers = headers,
                             files={
                                 'file': ("audio", audio_file, "audio/wav"),
                                 'model': "small"
                            })
    assert(response.status_code == 200)
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
    assert(response.status_code == 200)
    audio = response.content
    return io.BytesIO(audio)

def ask_llm_query(text):
    headers = {"Authorization": f"Bearer {api_key}"}
    response = requests.post(f"{base_url}/chat/completions",
                             headers = headers,
                             json={
                                 "model": "llama3:latest",
                                 "messages": [
                                    {
                                        "role": "developer",
                                        "content": "You are a helpful assistant, respond in one short sentence"
                                    },
                                    {
                                        "role": "user",
                                        "content": text
                                    }
                                ]
                            })
    assert(response.status_code == 200)
    response = response.json()
    text = response["choices"][0]["message"]["content"]
    text = text.replace('\n', ' ')
    text = text.replace('...', ',')
    return text

def search_music(phrase):
    cmd = f'yt-dlp --dump-json ytsearch1:"{phrase}"'
    output = subprocess.check_output(cmd, shell=True)
    config = json.loads(output.decode('utf-8'))
    return config

def download_music(config):
    cmd = f'yt-dlp --force-overwrite -f "ba" -x --audio-format vorbis {config["id"]} -o -'
    audio = subprocess.check_output(cmd, shell=True)
    return io.BytesIO(audio)

def get_weather():
    URL = 'https://api.weather.gov/gridpoints/BOX/69,91/forecast'
    response = requests.get(URL)
    forecast = response.json()['properties']
    period = forecast['periods'][0]
    return period["detailedForecast"]

def perform_task(intent, player):
    cmd = intent["command"]
    args = intent["arguments"]
    phrase = " ".join(args)

    if cmd == "stop" or len(phrase) == 0:
        return
    elif cmd == "play":
        config = search_music(phrase)
        out_phrase = f'Playing {config["title"]}.'
    elif cmd == "weather":
        out_phrase = get_weather()
    elif cmd == "repeat":
        out_phrase = phrase
    elif cmd == "llm":
        out_phrase = ask_llm_query(phrase)
    else:
        out_phrase = "Sorry, could you please repeat?"
    logging.info(("Output phrase:", out_phrase))
    sentences = re.split("[,!\.\?]", out_phrase)
    for chunk in sentences:
        if len(chunk) == 0:
            continue
        player.queue(get_audio_from_text(chunk))
    if cmd == "play":
        audio = download_music(config)
        player.queue(audio)

import struct
import numpy as np
import soundfile as sf
def save_audio(audio_data):
    with open("out.wav", "wb") as f:
        num_samples = len(audio_data) // 2  # assume 16-bit PCM
        waveform = np.array(struct.unpack(str(num_samples) + "H", audio_data)).astype(np.int16)
        sf.write(f, waveform, 16000)
def load_audio():
    with open("out.wav", "rb") as f:
        audio = f.read()
    return audio

if __name__ == "__main__":
    config = search_music("Swiss Portrait - Patagonia")
    print(download_music(config))
    #text = ask_llm_query("What is going on?")
    #text = get_weather()
    #audio = get_audio_from_text("hello")
    #save_audio(audio)
    #audio = load_audio()
    #text = get_text_from_audio(audio)
    print(text)
