import requests
from io import BytesIO
import numpy as np

from audio import AudioRecorder, AudioPlayer
from wakeword import WakewordDetector
from vad import VoiceActivityDetector
from logger import logging

min_record_time = 2.5 #seconds
beep_file = "beep.ogg"
#base_url = "http://localhost:5000/"
base_url = "https://hoodie.intern.farcry.pw/"

def request_handler(audio_file, player):
    response = requests.post(f"{base_url}/query", files={'audio': audio_file}).json()
    logging.info(("Query response:", str(response["intent"]), response["output"]))
    if response["intent"]["command"] == "stop":
        player.stop()
    for file_name in response["output"]:
        url = f"{base_url}/audio?file={file_name}"
        audio_file = BytesIO(requests.get(url).content)
        #audio_file = requests.get(url, stream=True).raw
        player.queue(audio_file)

def main():
    recorder = AudioRecorder()
    wakeword = WakewordDetector(recorder)
    voice_activity = VoiceActivityDetector(recorder)
    player = AudioPlayer()
    player.start()

    while True:
        try:
            logging.info("Waiting for wakeword")
            key = wakeword.wait_detect()
            logging.info(("Detected wakeword", key))
            player.queue(beep_file)

            logging.info("Recording audio")
            audio = voice_activity.wait_inactive(min_record_time)
            logging.info("Stopped recording audio")

            audio_file = BytesIO()
            recorder.write_file(audio_file, audio)
            audio_file.seek(0)

            logging.info("Sending request to server")
            request_handler(audio_file, player)
            logging.info("Received response from server")

        except KeyboardInterrupt:
            player.stop(close=True)
            player.join()
            return

if __name__ == "__main__":
    main()
