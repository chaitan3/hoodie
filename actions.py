import subprocess
import os, shutil, glob
import json
import io
from logger import logging
import re

import requests
from config import api_key, base_url
def get_text_from_audio(audio_file, model="small"):
    headers = {"Authorization": f"Bearer {api_key}"}
    response = requests.post(f"{base_url}/v1/audio/transcriptions",
                             headers = headers,
                             files={
                                 'file': ("audio", audio_file, "audio/wav"),
                                 'model': model
                            })
    assert(response.status_code == 200)
    response = response.json()
    text = response["text"]
    return text

def get_audio_from_text(text, model="tts-1"):
    headers = {"Authorization": f"Bearer {api_key}"}
    response = requests.post(f"{base_url}/v1/audio/speech",
                             headers = headers,
                             json={
                                 "input": text,
                                 "model": model
                            })
    assert(response.status_code == 200)
    audio = response.content
    return io.BytesIO(audio)

def ask_llm_query(prompt, model="llama3:latest"):
    headers = {"Authorization": f"Bearer {api_key}"}
    instruction = "You are a helpful assistant and you should respond in one short sentence."
    response = requests.post(f"{base_url}/chat/completions",
                             headers = headers,
                             json={
                                 "model": model,
                                 "messages": [
                                    {
                                        "role": "developer",
                                        "content": instruction
                                    },
                                    {
                                        "role": "user",
                                        "content": prompt
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
    output_file = "out"
    cmd = f'yt-dlp --force-overwrite -f "ba" -x --audio-format vorbis {config["id"]} -o {output_file}'
    subprocess.check_call(cmd, shell=True)
    output_file += ".ogg"
    #with open(output_file, "rb") as f:
    #    audio = f.read()
    #os.remove(output_file)
    #return io.BytesIO(output_file)
    return output_file

def get_weather():
    URL = 'https://api.weather.gov/gridpoints/BOX/69,91/forecast'
    response = requests.get(URL)
    forecast = response.json()['properties']
    period = forecast['periods'][0]
    return period["detailedForecast"]

def perform_task(intent, player):
    status = True
    cmd = intent["command"]
    args = intent["arguments"]
    phrase = " ".join(args)

    out_phrase = "Sorry, could you please repeat?"
    if len(phrase) == 0:
        if cmd == "stop":
            player.stop()
            return
        elif cmd == "weather":
            out_phrase = get_weather()
        else:
            status = False
    elif cmd == "repeat":
        out_phrase = phrase
    elif cmd == "play":
        config = search_music(phrase)
        out_phrase = f'Playing {config["title"]}.'
    elif cmd == "llm":
        out_phrase = ask_llm_query(phrase)
    else:
        status = False
    logging.info(("Output phrase:", out_phrase))
    sentences = re.split("[,!\.\?]", out_phrase)
    for chunk in sentences:
        if len(chunk) == 0:
            continue
        player.queue(get_audio_from_text(chunk))
    if cmd == "play":
        audio = download_music(config)
        player.queue(audio)
    return status


if __name__ == "__main__":
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

    import sys
    #config = search_music("Swiss Portrait - Patagonia")
    #print(download_music(config))
    #text = ask_llm_query("What is going on?")
    #text = get_weather()
    #audio = get_audio_from_text("hello")
    #save_audio(audio)
    #audio = load_audio()
    text = get_text_from_audio(sys.argv[1], model="small")
    print(text)
