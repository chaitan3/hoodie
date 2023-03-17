import numpy as np
import soundfile as sf
import sounddevice as sd
from threading import Thread

from logger import logging

sleep_time = 100 #ms

# desktop
input_device_id = 10
output_device_id = 11
blocksize = 0
input_sample_rate = 16000
input_channels = 1

# raspi
#input_device_id = 1
#output_device_id = 0
#blocksize = 16384
#input_samp_rate = 48000
#input_channels = 1

class AudioRecorder(object):
    def __init__(self):
        self.sample_rate = input_sample_rate
        
    def read_callback(self, callback, frames):
        with sd.InputStream(device=input_device_id, samplerate=self.sample_rate,
                           channels=input_channels,
                           dtype=np.int16,
                           callback=callback,
                           blocksize=frames) as stream:
            while stream.active:
               sd.sleep(sleep_time)

    def write_file(self, file_obj, audio):
        sf.write(file_obj, audio, self.sample_rate, format="WAV")

class AudioPlayer(Thread):
    def __init__(self):
        Thread.__init__(self)
        self._reset()
        self.active = True

    def queue(self, file_obj):
        self.files.append(file_obj)

    def stop(self, close=False):
        self.active = not close
        self.play = False

    def run(self):
        while self.active:
            try:
                file_obj = self.files[0]
                self._play_file(file_obj)
                self.files.pop(0)
                if not self.play:
                    self._reset()
            except IndexError:
                sd.sleep(sleep_time)

    def _reset(self):
        self.files = []
        self.play = True

    def _play_file(self, file_obj):
        with sf.SoundFile(file_obj) as audio_file:
            dtype = np.float32
            def callback(outdata, frame_count, time, status):
                data = audio_file.read(frame_count, out=outdata, dtype=dtype)
                if (data.size != outdata.size):
                    outdata[data.shape[0]:,:].fill(0.0)
                    raise sd.CallbackStop()
            with sd.OutputStream(device=output_device_id, samplerate=audio_file.samplerate,
                               channels=audio_file.channels,
                               dtype=dtype,
                               callback=callback,
                               blocksize=blocksize) as stream:
                while stream.active and self.play:
                   sd.sleep(sleep_time)

if __name__ == "__main__":
    print(sd.query_devices())
    #from wakeword import wait_for_wakeword
    #wait_for_wakeword()
    #record_audio_to_bytes()
    #wait_for_wakeword()
    #record_audio_to_bytes()

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
    #import librosa
    #audio = librosa.resample(record_audio(3), orig_sr=device_samp_rate, target_sr=16000)
    #soundfile.write("test.wav", audio, 16000)
    #from whisper import get_text_from_audio
    #print(get_text_from_audio(audio))

    #import sys
    #play_audio_from_file(sys.argv[1])
    #record_audio_to_file("sd.wav")

    #audio, wav_rate = librosa.load("audiofile10Seconds.wav")
    #audio = audio[:,0]
    #audio = librosa.resample(audio, orig_sr=wav_rate, target_sr=device_samp_rate)
    #play_audio(audio)
