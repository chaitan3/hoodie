import numpy as np
import struct
#import pvporcupine
import sounddevice as sd

from logger import logging

class WakewordDetector(object):
    def __init__(self, recorder):
        self.porcupine = pvporcupine.create(keywords=pvporcupine.KEYWORDS)
        self.recorder = recorder

    def wait_detect(self):
        ratio = self.recorder.sample_rate // self.porcupine.sample_rate
        frames = self.porcupine.frame_length * ratio

        res = None
        def callback(indata, frame_count, time, status):
            nonlocal res
            audio = indata[::ratio].copy()
            pcm = struct.unpack_from("h" * self.porcupine.frame_length, audio)
            res = self.porcupine.process(pcm)
            if res >= 0:
                raise sd.CallbackStop()
        self.recorder.read_callback(callback, frames)
        return list(pvporcupine.KEYWORDS)[res]

    def __del__(self):
        self.porcupine.delete()

if __name__ == "__main__":
    wait_for_wakeword()
