import numpy as np
import soundfile as sf
import sounddevice as sd
from threading import Thread

from logger import logging

# desktop
input_device_id = 10
output_device_id = 11
blocksize = 0
input_samp_rate = 16000
input_channels = 1

# raspi
#input_device_id = 1
#output_device_id = 0
#blocksize = 16384
#input_samp_rate = 48000
#input_channels = 1

class AudioRecorder(object):
    def __init__(self):
        self.sample_rate = input_samp_rate
        self.stream = sd.InputStream(device=input_device_id,
                       samplerate=input_samp_rate,
                       channels=input_channels,
                       dtype=np.int16)
        self.stream.start()

    def read(self, frames=None, frame_time=None):
        if frame_time is not None:
            frames = int(frame_time * self.sample_rate)
        audio, overflow = self.stream.read(frames=frames)
        if overflow:
            logging.warn("Overflow detected on mic")
        return audio

    def write(self, file_obj, audio):
        sf.write(file_obj, audio, self.sample_rate, format="WAV")

    def __del__(self):
        self.stream.stop()
        self.stream.close()

class AudioPlayer(Thread):
    def __init__(self):
        Thread.__init__(self)
        self._reset()

    def queue(self, file_obj):
        self.files.append(file_obj)

    def stop(self):
        self.active = False

    def run(self):
        while True:
            try:
                self._play(self.files.pop(0))
                if not self.active:
                    self._reset()
            except IndexError:
                sd.sleep(100)

    def _reset(self):
        self.files = []
        self.active = True

    def _play(self, file_obj):
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
                while stream.active and self.active:
                   sd.sleep(int(0.1*1000))

if __name__ == "__main__":
    print(sd.query_devices())
    #from wakeword import wait_for_wakeword
    #wait_for_wakeword()
    #record_audio_to_bytes()
    #wait_for_wakeword()
    #record_audio_to_bytes()

    a = AudioPlayer()
    a.start()
    a.queue("test2.ogg")
    a.queue("test3.ogg")
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
