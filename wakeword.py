import numpy as np
import struct
import pvporcupine
import sounddevice as sd

from logger import logging

class WakewordDetector(object):
    def __init__(self):
        self.porcupine = pvporcupine.create(keywords=pvporcupine.KEYWORDS)

    def get_detector(self, sample_rate):
        ratio = sample_rate // self.porcupine.sample_rate
        frames = self.porcupine.frame_length * ratio

        def callback(indata, frame_count, time, status):
            audio = indata[::ratio].copy()
            pcm = struct.unpack_from("h" * self.porcupine.frame_length, audio)
            res = self.porcupine.process(pcm)
            if res >= 0:
                logging.info(("Detected wakeword", list(pvporcupine.KEYWORDS)[res]))
                raise sd.CallbackStop()
        return callback, frames

    def __del__(self):
        self.porcupine.delete()

if __name__ == "__main__":
    wait_for_wakeword()
