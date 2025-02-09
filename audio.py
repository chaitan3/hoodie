import numpy as np
import soundfile as sf
import sounddevice as sd
import resampy
import io

from threading import Thread
from logger import logging

sleep_time = 50 #ms

# desktop
input_device_id = 4
output_device_id = 8
blocksize = 0
input_sample_rate = 16000
input_channels = 1
output_sample_rate = 44100

# raspi
#input_device_id = 3
#output_device_id = 2
#blocksize = 0
#input_sample_rate = 48000
#input_channels = 1

class AudioRecorder(object):
    def __init__(self):
        self.sample_rate = input_sample_rate
        
    def read_callback(self, callback, frames):
        with sd.InputStream(device=input_device_id, samplerate=self.sample_rate,
                           channels=input_channels,
                           latency=0.1,
                           dtype=np.int16,
                           callback=callback,
                           blocksize=frames) as stream:
            while stream.active:
               sd.sleep(sleep_time)

    def write_file(self, audio_file, audio):
        sf.write(audio_file, audio, self.sample_rate, format="WAV")

class AudioPlayer(Thread):
    def __init__(self):
        Thread.__init__(self)
        self._reset()
        self.active = True
        self.stream = None

    def queue(self, audio_file):
        self.files.append(audio_file)

    def stop(self, close=False):
        self.active = not close
        self.play = False

    def run(self):
        while self.active:
            try:
                audio_file = self.files[0]
                self._play_file(audio_file)
                self.files.pop(0)
                if not self.play:
                    self._reset()
            except IndexError:
                self.stream = None
                sd.sleep(sleep_time)

    def _reset(self):
        self.files = []
        self.play = True

    def _play_file(self, audio_file):
        data, samplerate = sf.read(audio_file)  # Load with original sample rate
        if samplerate != output_sample_rate:
            output_data = resampy.resample(data, samplerate, output_sample_rate)  # Resample to 16kHz
            audio_file = io.BytesIO()
            sf.write(audio_file, output_data, output_sample_rate, format="WAV")
            audio_file.seek(0)
        with sf.SoundFile(audio_file) as audio_obj:
            dtype = np.float32
            def callback(outdata, frame_count, time, status):
                data = audio_obj.read(frame_count, out=outdata, dtype=dtype)
                if (data.size != outdata.size):
                    outdata[data.shape[0]:,:].fill(0.0)
                    raise sd.CallbackStop()
            if self.stream is None:
                self.stream = sd.OutputStream(device=output_device_id, samplerate=audio_obj.samplerate,
                           channels=audio_obj.channels,
                           dtype=dtype,
                           callback=callback,
                           blocksize=blocksize)
            while self.stream.active and self.play:
               sd.sleep(sleep_time)

if __name__ == "__main__":
    print(sd.query_devices())
    exit(0)

    #recorder = AudioRecorder()

    a = AudioPlayer()
    a.start()
    #from urllib.request import urlopen
    #import requests
    #url = "http://tinyurl.com/shepard-risset"
    #a.queue(urlopen(url))
    #a.queue(requests.get(url, stream=True).raw)
    a.queue("beep.ogg")
    #a.queue("test2.ogg")
    #a.queue("test3.ogg")
    #a.queue("beep.ogg")
    while len(a.files) > 0:
        sd.sleep(sleep_time)
    a.stop(close=True)
    a.join()
