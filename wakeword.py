import numpy as np
import subprocess
import sys
import struct
import pvporcupine

from logger import logging

class WakewordDetector(object):
    def __init__(self):
        self.porcupine = pvporcupine.create(keywords=pvporcupine.KEYWORDS)

    def detect(self, recorder):
        ratio = recorder.sample_rate // self.porcupine.sample_rate
        audio = None
        while True:
            frames = self.porcupine.frame_length * ratio
            audio = recorder.read(frames=frames)
            audio = audio[::ratio].copy()
            pcm = struct.unpack_from("h" * self.porcupine.frame_length, audio)
            res = self.porcupine.process(pcm)
            if res >= 0:
                logging.info(("Detected wakeword", list(pvporcupine.KEYWORDS)[res]))
                break

    def __del__(self):
        self.porcupine.delete()

if __name__ == "__main__":
    wait_for_wakeword()
