import requests
from io import BytesIO

from audio import record_audio_to_file, play_audio_from_file
from wakeword import wait_for_wakeword
from logger import logging

def send_audio_to_server(audio_file):
    #url = "http://localhost:5000"
    url = "https://hoodie.intern.farcry.pw"

    return requests.post(f"{url}/receive_and_respond", files={'audio': audio_file})

def main():
    listen_time = 2
    while True:
        audio_file = BytesIO()
        logging.info("Waiting for wakeword")
        wait_for_wakeword()
        record_audio_to_file(audio_file, listen_time)
        logging.info("Recorded audio")
        audio_file.seek(0)
        resp = send_audio_to_server(audio_file)
        logging.info("Received response from server")
        play_audio_from_file(BytesIO(resp.content))

if __name__ == "__main__":
    main()
