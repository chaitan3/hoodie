import requests
from io import BytesIO

from audio import record_audio_to_file, play_audio_from_file
from wakeword import wait_for_wakeword

def send_audio_to_server(audio_file):
    #url = "http://localhost:5000"
    url = "https://hoodie.intern.farcry.pw"

    return requests.post(f"{url}/receive_and_respond", files={'audio': audio_file})

def main():
    listen_time = 2
    while True:
        audio_file = BytesIO()
        print("Waiting for wakeword", pvporcupine.KEYWORDS[res])
        wait_for_wakeword()
        record_audio_to_file(audio_file, listen_time)
        print("Recorded audio", pvporcupine.KEYWORDS[res])
        audio_file.seek(0)
        resp = send_audio_to_server(audio_file)
        print("Received response from server", pvporcupine.KEYWORDS[res])
        play_audio_from_file(BytesIO(resp.content))

if __name__ == "__main__":
    main()
