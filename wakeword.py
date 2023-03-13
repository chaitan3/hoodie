from audio import input_device_id, input_samp_rate, input_dtype
import sounddevice as sd
import soundfile as sf
import pvporcupine

import numpy as np
import subprocess
import sys
import struct

def wait_for_wakeword():
    #porcupine = pvporcupine.create(keywords=["alexa"], sensitivities=[0.3])
    porcupine = pvporcupine.create(keywords=pvporcupine.KEYWORDS)
    assert input_samp_rate == porcupine.sample_rate
    print(pvporcupine.KEYWORDS)
    print(porcupine.frame_length)
    print(porcupine.sample_rate)
    frames = []
    while True:
        audio = sd.rec(device=input_device_id,
                       frames=porcupine.frame_length,
                       samplerate=porcupine.sample_rate,
                       channels=2,
                       dtype=np.int16)
        sd.wait()
        frames.append(audio.copy())
        pcm = struct.unpack_from("h" * porcupine.frame_length, audio)
        res = porcupine.process(pcm)
        if res >= 0:
            print("Detected wakeword", pvporcupine.KEYWORDS[res])
            porcupine.delete()
            return audio.astype(input_dtype)

if __name__ == "__main__":
    wait_for_wakeword()
