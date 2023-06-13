import numpy as np
import struct
import sounddevice as sd
from openwakeword.model import Model

from logger import logging

class WakewordDetector(object):
    def __init__(self, recorder):
        self.detector = Model()
        self.sample_rate = 16000
        self.frame_length = 1280
        self.recorder = recorder
        self.keyword = "alexa"
        self.threshold = 0.8
        self.skip_frames = 10

    def wait_detect(self):
        ratio = self.recorder.sample_rate // self.sample_rate
        frames = self.frame_length * ratio

        res = None
        self.count = 0
        def callback(indata, frame_count, time, status):
            self.count += 1
            nonlocal res
            audio = indata[::ratio].copy()
            pcm = struct.unpack_from("h" * self.frame_length, audio)
            res = self.detector.predict(pcm)[self.keyword]
            if res >= self.threshold and self.count > self.skip_frames:
                self.detector.reset()
                raise sd.CallbackStop()
        self.recorder.read_callback(callback, frames)
        return self.keyword

if __name__ == "__main__":
    pass
