import requests
from io import BytesIO
import numpy as np

from audio import AudioRecorder, AudioPlayer
from wakeword import WakewordDetector
from vad import VoiceActivityDetector
from logger import logging

min_listen_time = 2 #seconds
beep_file = "beep.ogg"
#url = "http://localhost:5000/query"
url = "https://hoodie.intern.farcry.pw/query"

def main():
    wakeword = WakewordDetector()
    activity = VoiceActivityDetector()
    recorder = AudioRecorder()
    player = AudioPlayer()
    player.start()

    while True:
        try:
            logging.info("Waiting for wakeword")
            recorder.read_callback(*wakeword.get_detector(recorder.sample_rate))
            player.queue(beep_file)

            logging.info("Recording audio")
            frames = []
            frames.append(recorder.read(min_listen_time * recorder.sample_rate))
            # add frames
            recorder.read_callback(*activity.get_detector(recorder.sample_rate))

            audio_file = BytesIO()
            recorder.write_file(audio_file, np.concatenate(frames))
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
