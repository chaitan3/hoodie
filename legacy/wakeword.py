import numpy as np
import struct
import sounddevice as sd
from openwakeword.model import Model

from logger import logging
from config import wake_keyword

class WakewordDetector(object):
    def __init__(self, recorder):
        self.detector = Model()
        self.sample_rate = 16000 # Hz
        self.frame_length = 160 * 16 # (ms) * (frames / ms)
        self.buffer_frames = 4
        self.recorder = recorder
        self.keyword = wake_keyword
        self.threshold = 0.5
        self.skip_frames = 10

    def wait_detect(self):
        self.count = 0
        ratio = self.recorder.sample_rate // self.sample_rate
        block_size = self.frame_length * ratio
        buf_size = block_size*2

        buf_idx = 0
        buffer = np.zeros(buf_size, np.int16)
        wake_audio = [None]*self.buffer_frames

        def callback(indata, frame_count, time, status):
            nonlocal buf_idx, wake_audio, buffer
            buffer[buf_idx:buf_idx + len(indata)] = indata.flatten()
            buf_idx += len(indata)
            if status.input_overflow:
                print("WARNING: input overflow in recorder")
            if buf_idx >= block_size:
                self.count += 1
                pcm = buffer[:block_size:ratio]
                res = self.detector.predict(pcm)[self.keyword]
                #print("prediction", res)
                wake_audio.pop(0)
                wake_audio.append(buffer[:block_size].copy())
                if res >= self.threshold and self.count > self.skip_frames:
                    self.detector.reset()
                    raise sd.CallbackStop()

                buffer[:buf_idx-block_size] = buffer[block_size:buf_idx]
                buf_idx -= block_size
        self.recorder.read_callback(callback, 0)
        return self.keyword, np.concatenate(wake_audio)

if __name__ == "__main__":
    pass
