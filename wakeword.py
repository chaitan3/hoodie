from audio import input_device_id
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
    with sd.InputStream(device=input_device_id,
                       samplerate=porcupine.sample_rate,
                       channels=1,
                        dtype=np.int16) as stream:
        audio = None
        while True:
            audio, overflow = stream.read(frames=porcupine.frame_length)
            assert not overflow
            audio = audio[:].copy()
            pcm = struct.unpack_from("h" * porcupine.frame_length, audio)
            res = porcupine.process(pcm)
            if res >= 0:
                print("Detected wakeword", list(pvporcupine.KEYWORDS)[res])
                break
    porcupine.delete()

if __name__ == "__main__":
    wait_for_wakeword()
