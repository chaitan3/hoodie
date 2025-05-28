import numpy as np
import soundfile as sf
import sounddevice as sd
import resampy
import io

from threading import Thread
from logger import logging
import time

sleep_time = 50 #ms
timeout_time = 10 #s

# desktop
input_device_id = 10
output_device_id = 11
blocksize = 0
input_sample_rate = 48000
input_channels = 1
output_sample_rate = 44100

# raspi
#input_device_id = 3
#output_device_id = 2
#blocksize = 0
#input_sample_rate = 48000
#input_channels = 1
#output_sample_rate = 44100
#
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
        self.audio_stream = None

    def queue(self, audio_file):
        self.files.append(audio_file)

    def stop(self, close=False):
        self.active = not close
        self.play = False

    def run(self):
        start_time = time.time()
        while self.active:
            try:
                audio_file = self.files.pop(0)
                start_time = time.time()
                self._play_file(audio_file)
                if not self.play:
                    self._reset()
            except IndexError:
                curr_time = time.time()
                if self.stream and (curr_time - start_time) > timeout_time:
                    self.stream.close()
                    self.stream = None
                    logging.info("Closed audio output stream")
                sd.sleep(sleep_time)

    def _reset(self):
        self.files = []
        self.play = True
        self.stream = None
        self.audio_stream = None

    def _open_stream(self):
        dtype = np.float32
        def callback(outdata, frame_count, time, status):
            if self.audio_stream:
                data = self.audio_stream.read(frame_count, out=outdata, dtype=dtype)
                if (data.size != outdata.size):
                    outdata[data.shape[0]:,:].fill(0.0)
                    self.audio_stream = None
            else:
                outdata[:,:].fill(0.0)
        logging.info("Opened audio output stream")
        self.stream = sd.OutputStream(device=output_device_id, samplerate=self.audio_stream.samplerate,
                               channels=self.audio_stream.channels,
                               dtype=dtype,
                               callback=callback,
                               blocksize=blocksize)
        self.stream.start()

    def _play_file(self, audio_file):
        data, samplerate = sf.read(audio_file)  # Load with original sample rate
        if samplerate != output_sample_rate:
            output_data = resampy.resample(data, samplerate, output_sample_rate)  # Resample to 16kHz
            audio_file = io.BytesIO()
            sf.write(audio_file, output_data, output_sample_rate, format="WAV")
            audio_file.seek(0)
        self.audio_stream = sf.SoundFile(audio_file)
        if self.stream is None:
            self._open_stream()
        while self.audio_stream and self.play:
           sd.sleep(sleep_time)

if __name__ == "__main__":
    print(sd.query_devices())

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
    a.queue("beep.ogg")
    while len(a.files) > 0:
        sd.sleep(sleep_time)
    a.stop(close=True)
    a.join()
