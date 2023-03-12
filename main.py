import requests
from io import BytesIO

from audio import record_audio_to_file, play_audio_from_file, input_device_name
from wakeword import wait_for_wakeword

def send_audio_to_server(audio_file):
    url = "http://localhost:5000"
    return requests.post(f"{url}/receive_and_respond", files={'audio': audio_file})

def main():
    listen_time = 2
    while True:
        #wait_for_wakeword()

        audio_file = BytesIO()
        record_audio_to_file(audio_file, listen_time)
        resp = send_audio_to_server(audio_file)
        play_audio_from_file(BytesIO(resp.content))

if __name__ == "__main__":
    main()
