from audio import input_device_id
from audio import input_samp_rate, input_channels
import sounddevice as sd
import soundfile as sf
import pvporcupine

import numpy as np
import subprocess
import sys
import struct

def wait_for_wakeword():
    #porcupine = pvporcupine.create(keywords=["alexa"])
    porcupine = pvporcupine.create(keywords=pvporcupine.KEYWORDS)
    ratio = input_samp_rate // porcupine.sample_rate
    assert input_channels == 1
    assert ratio * porcupine.sample_rate == input_samp_rate
    with sd.InputStream(device=input_device_id,
                       samplerate=input_samp_rate,
                       channels=input_channels,
                        dtype=np.int16) as stream:
        audio = None
        while True:
            frames = porcupine.frame_length * ratio
            audio, overflow = stream.read(frames=frames)
            audio = audio[::ratio].copy()
            pcm = struct.unpack_from("h" * porcupine.frame_length, audio)
            res = porcupine.process(pcm)
            if overflow:
                print("Overflow detected on mic")
            if res >= 0:
                print("Detected wakeword", list(pvporcupine.KEYWORDS)[res])
                break
    porcupine.delete()

if __name__ == "__main__":
    wait_for_wakeword()
