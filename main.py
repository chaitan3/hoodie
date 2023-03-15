import requests
from io import BytesIO

from audio import AudioRecorder, AudioPlayer
from wakeword import WakewordDetector
from vad import VoiceActivityDetector
from logger import logging

min_listen_time = 2 #seconds
beep_file = "beep.ogg"

def send_audio_to_server(audio_file):
    #url = "http://localhost:5000"
    url = "https://hoodie.intern.farcry.pw"

    return requests.post(f"{url}/query", files={'audio': audio_file})

def main():
    wakeword = WakewordDetector()
    activity = VoiceActivityDetector()
    recorder = AudioRecorder()
    player = AudioPlayer()
    player.start()

    while True:
        logging.info("Waiting for wakeword")
        wakeword.detect(recorder)
        player.queue(beep_file)

        audio_file = BytesIO()
        audio = recorder.read(frame_time=min_listen_time)
        recorder.write(audio_file, audio)
        while (audio := activity.detect(recorder)):
            recorder.write(audio_file, audio)
        logging.info("Recorded audio")
        audio_file.seek(0)

        resp = send_audio_to_server(audio_file)
        logging.info("Received response from server")
        print(resp, len(resp.content), type(resp.content))
        player.queue(BytesIO(resp.content))

if __name__ == "__main__":
    main()
