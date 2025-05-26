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

import inflect
converter = inflect.engine()

def get_audio_from_text(text, model="tts-1"):
    text = re.sub(r"(\d+(?:\.\d+)?)", lambda x: converter.number_to_words(x.group(0)), text)
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

def ask_llm_query(prompt, model="qwen3:14b"):
    headers = {"Authorization": f"Bearer {api_key}"}
    instruction = "You are a helpful assistant and you should respond in one sentence."
    response = requests.post(f"{base_url}/chat/completions",
                             headers = headers,
                             json={
                                 "model": model,
                                 #"features": {"web_search": True},
                                 "messages": [
                                    {
                                        "role": "system",
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

def ask_llm_query_tool(prompt, model="qwen3:4b"):
    headers = {"Authorization": f"Bearer {api_key}"}
    instruction = "You are a helpful assistant and you should respond in one sentence."
    response = requests.post(f"{base_url}/chat/completions",
                             headers = headers,
                             json={
                                 "model": model,
                                 #"features": {"web_search": True},
                                 "messages": [
                                    {
                                        "role": "system",
                                        "content": instruction
                                    },
                                    {
                                        "role": "user",
                                        "content": prompt
                                    }
                                ],
                                "tools": [{
                                  'type': 'function',
                                  'function': {
                                    'name': 'get_current_weather',
                                    'description': 'Get the current weather for a city',
                                    'parameters': {
                                      'type': 'object',
                                      'properties': {
                                        'city': {
                                          'type': 'string',
                                          'description': 'The name of the city',
                                        },
                                      },
                                      'required': ['city'],
                                    },
                                  },
                                }],
                            })
    assert(response.status_code == 200)
    response = response.json()
    print(response)
    message = response["choices"][0]["message"]
    tool_call = message["tool_calls"][0]
    tool_call_id = tool_call["id"]
    text = get_weather()
    print(text)
    response = requests.post(f"{base_url}/chat/completions",
                             headers = headers,
                             json={
                                 "model": model,
                                 #"features": {"web_search": True},
                                 "messages": [
                                    {
                                        "role": "system",
                                        "content": instruction
                                    },
                                    {
                                        "role": "user",
                                        "content": prompt
                                    },
                                    message,
                                    {
                                        "role": "tool",
                                        "tool_call_id": tool_call["id"],
                                        "content": text
                                    }
                                ],
                                "tools": [{
                                  'type': 'function',
                                  'function': {
                                    'name': 'get_current_weather',
                                    'description': 'Get the current weather for a city',
                                    'parameters': {
                                      'type': 'object',
                                      'properties': {
                                        'city': {
                                          'type': 'string',
                                          'description': 'The name of the city',
                                        },
                                      },
                                      'required': ['city'],
                                    },
                                  },
                                }],
                            })
    assert(response.status_code == 200)
    response = response.json()
    text = response["choices"][0]["message"]["content"]
    print(response)
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

import datetime
def get_time():
    now = datetime.datetime.now()
    hours = str(now.hour % 12 or 12)  # Display noon and midnight correctly
    minutes = now.minute
    am_pm = 'AM' if now.hour < 12 else 'PM'
    return f"The time is {hours} {minutes:02d} {am_pm}"


def perform_task(intent, player):
    status = True
    cmd = intent["command"]
    args = intent["arguments"]
    phrase = " ".join(args)

    out_phrase = ""
    if cmd == "false_positive":
        status = False
    elif len(phrase) == 0:
        if cmd == "stop":
            player.stop()
            return
        elif cmd == "weather":
            out_phrase = get_weather()
        elif cmd == "time":
            out_phrase = get_time()
        else:
            out_phrase = "Sorry, could you please repeat?"
            status = False
    elif cmd == "repeat":
        out_phrase = phrase
    elif cmd == "play":
        config = search_music(phrase)
        out_phrase = f'Playing {config["title"]}.'
    elif cmd == "llm":
        out_phrase = ask_llm_query(phrase)
    else:
        out_phrase = "Sorry, could you please repeat?"
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
        audio_data = audio_data.read()
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
    #text = ask_llm_query("/nothink What is going on?")
    #text = ask_llm_query("How long was the Ancient Egyptian Civilization active?")
    text = ask_llm_query_tool("/nothink what is the temperature in boston right now?")
    print(text)
    #Eprint(get_time())
    #text = get_weather()
    #audio = get_audio_from_text("what is 2.2 multiplied by 4?")
    #save_audio(audio)
    #audio = load_audio()
    #text = get_text_from_audio(sys.argv[1], model="small")
    #print(text)
