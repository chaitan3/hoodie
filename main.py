import requests
import numpy as np
from logger import logging
import io

from audio import AudioRecorder, AudioPlayer
from wakeword import WakewordDetector
from vad import VoiceActivityDetector

from actions import perform_task, get_text_from_audio
from intent import parse_text

from config import api_key

min_record_time = 2.5 #seconds
beep_file = "beep.ogg"

def request_handler(audio_file, player):
    text = get_text_from_audio(audio_file)
    intent = parse_text(text)
    status = perform_task(intent, player)
    return status
    
       
def main():
    failure_count = 0
    recorder = AudioRecorder()
    wakeword = WakewordDetector(recorder)
    voice_activity = VoiceActivityDetector(recorder)
    player = AudioPlayer()
    player.start()

    while True:
        try:
            logging.info("Waiting for wakeword")
            key, wake_audio = wakeword.wait_detect()
            logging.info(("Detected wakeword", key))
            player.queue(beep_file)

            logging.info("Recording audio")
            audio = voice_activity.wait_inactive(min_record_time)
            logging.info("Stopped recording audio")

            audio_file = io.BytesIO()
            audio_data = np.concatenate((wake_audio, audio))
            recorder.write_file(audio_file, audio_data)
            audio_file.seek(0)

            logging.info("Sending request to server")
            status = request_handler(audio_file, player)
            logging.info("Received response from server")
            if not status:
                with open(f"failed_audio_{failure_count}.wav", "wb") as out_file:
                    sf.write(f, audio_data, recorder.sample_rate, format="WAV")
                failure_count += 1

        except KeyboardInterrupt:
            player.stop(close=True)
            player.join()
            return

if __name__ == "__main__":
    main()
