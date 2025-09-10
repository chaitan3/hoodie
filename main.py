import requests
import numpy as np
from logger import logging
import io
import soundfile as sf

from audio import AudioRecorder, AudioPlayer
from wakeword import WakewordDetector
from vad import VoiceActivityDetector
from agent import Assistant

from actions import perform_task, get_text_from_audio

import config

min_record_time = 2.5 #seconds
beep_file = "beep.ogg"
agent = Assistant.create_agent()

def request_handler(audio_file, player):
    text = get_text_from_audio(audio_file)
    status = perform_task(text, agent, player)
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
            #player.queue(beep_file)
            
            logging.info("Recording audio")
            audio = voice_activity.wait_inactive(min_record_time)
            logging.info("Stopped recording audio")

            audio_file = io.BytesIO()
            recorder.write_file(audio_file, wake_audio)
            audio_file.seek(0)
            wake_text = get_text_from_audio(audio_file)
            if wake_text.strip().lower() != config.wake_keyword:
                logging.info("Invalid wake up from sleep, prediction: {}".format(wake_text))
                continue

            audio_file = io.BytesIO()
            recorder.write_file(audio_file, audio)
            audio_file.seek(0)

            logging.info("Sending request to server")
            status = request_handler(audio_file, player)
            logging.info("Received response from server")
            if not status:
                with open(f"failed_audio_{failure_count}.wav", "wb") as out_file:
                    sf.write(out_file, audio_data, recorder.sample_rate, format="WAV")
                failure_count += 1

        except KeyboardInterrupt:
            player.stop(close=True)
            player.join()
            return

if __name__ == "__main__":
    main()
