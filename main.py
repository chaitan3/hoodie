import requests

from audio import record_audio_to_bytes, play_audio_from_bytes, input_device_name
from wakeword import wait_for_wakeword

def send_audio_to_server(audio_file):
    url = "http://localhost:5000"
    return requests.post(f"{url}/receive_and_respond", files={'audio': audio_file})

def main():
    listen_time = 3
    while True:
        wait_for_wakeword()

        audio_file = record_audio_to_bytes(listen_time)
        resp = send_audio_to_server(audio_file)
        play_audio_from_bytes(resp.content)

if __name__ == "__main__":
    main()
