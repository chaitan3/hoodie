import requests
from io import BytesIO
import numpy as np

from audio import AudioRecorder, AudioPlayer
from wakeword import WakewordDetector
from vad import VoiceActivityDetector
from logger import logging

min_record_time = 2 #seconds
beep_file = "beep.ogg"
#url = "http://localhost:5000/query"
url = "https://hoodie.intern.farcry.pw/query"

def main():
    recorder = AudioRecorder()
    wakeword = WakewordDetector(recorder)
    voice_activity = VoiceActivityDetector(recorder)
    player = AudioPlayer()
    player.start()

    while True:
        try:
            logging.info("Waiting for wakeword")
            wakeword.wait_detect()
            player.queue(beep_file)

            logging.info("Recording audio")
            audio = voice_activity.wait_inactive(min_record_time)
            logging.info("Stopped recording audio")

            audio_file = BytesIO()
            recorder.write_file(audio_file, audio)
            audio_file.seek(0)

            logging.info("Sending request to server")
            resp = requests.post(url, files={'audio': audio_file})
            logging.info("Received response from server")
            player.queue(BytesIO(resp.content))

        except KeyboardInterrupt:
            player.stop(close=True)
            player.join()
            return

if __name__ == "__main__":
    main()
