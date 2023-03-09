import requests
import soundfile as sf
from io import BytesIO

from audio import record_audio, play_audio, device_samp_rate

#audio_file = open('audiofile10Seconds.wav', 'rb')
def record_audio_to_bytes():
    audio_file = BytesIO()
    sf.write(audio_file, record_audio(3), device_samp_rate, format="WAV")
    audio_file.seek(0)
    return audio_file

def play_audio_from_bytes(audio):
    audio_file = BytesIO(audio)
    audio, _ = sf.read(audio_file)
    play_audio(audio)

audio_file = record_audio_to_bytes()

files = {'audio': audio_file}
resp = requests.post("http://localhost:5000/receive_and_respond", files=files)
play_audio_from_bytes(resp.content)
