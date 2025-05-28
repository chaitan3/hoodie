import datetime
import requests
import subprocess
import json

from pydantic_ai import RunContext, Agent
from dataclasses import dataclass

@dataclass
class ToolUsage:
    music_config: dict

def get_time():
    now = datetime.datetime.now()
    hours = str(now.hour % 12 or 12)  # Display noon and midnight correctly
    minutes = now.minute
    am_pm = 'AM' if now.hour < 12 else 'PM'
    return f"The time is {hours} {minutes:02d} {am_pm}"

def get_weather():
    URL = 'https://api.weather.gov/gridpoints/BOX/69,91/forecast'
    response = requests.get(URL)
    forecast = response.json()['properties']
    period = forecast['periods'][0]
    return period["detailedForecast"]

def play_music(ctx: RunContext[ToolUsage], phrase: str):
    cmd = f'yt-dlp --dump-json ytsearch1:"{phrase}"'
    output = subprocess.check_output(cmd, shell=True)
    config = json.loads(output.decode('utf-8'))
    #audio = download_music(config)
    #player.enqueue(audio)
    ctx.deps.music_config = config
    return config["title"]

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
