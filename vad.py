import webrtcvad
import sounddevice as sd

from logger import logging

vad_frame_time = 0.03 # ms

class VoiceActivityDetector(object):
    def __init__(self):
        self.vad = webrtcvad.Vad()

    def get_detector(self, sample_rate):
        def callback(indata, frame_count, time, status):
            res = self.vad.is_speech(indata.tobytes(), sample_rate)
            if res:
                logging.info("Stopped recording audio")
                raise sd.CallbackStop()
        return callback, int(vad_frame_time * sample_rate)
