import subprocess
import os, shutil, glob
import json
import io
from logger import logging
import re
import tools

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

def ask_llm_query(prompt, model="qwen3:4b"):
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

def perform_task(phrase, agent, player):
    logging.info(("Input phrase", phrase))
    out_phrase = agent(phrase).output
    out_phrase = out_phrase.replace("<think>\n\n</think>\n\n", "")
    logging.info(("Output phrase", out_phrase))
    player.queue(get_audio_from_text(out_phrase))
    if len(agent.deps.music_config) > 0:
        audio = tools.download_music(agent.deps.music_config)
        player.queue(audio)
        agent.deps.music_config = {}
    return True

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
    text = ask_llm_query("How long was the Ancient Egyptian Civilization active?")
    text = ask_llm_query("What is the time right now?")
    print(text)
    #Eprint(get_time())
    #text = get_weather()
    #audio = get_audio_from_text("what is 2.2 multiplied by 4?")
    #save_audio(audio)
    #audio = load_audio()
    #text = get_text_from_audio(sys.argv[1], model="small")
    #print(text)
