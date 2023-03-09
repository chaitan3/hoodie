import requests

from audio import record_audio_to_bytes, play_audio_from_bytes

audio_file = record_audio_to_bytes()

resp = requests.post("http://localhost:5000/receive_and_respond",
                     files={'audio': audio_file})

play_audio_from_bytes(resp.content)
