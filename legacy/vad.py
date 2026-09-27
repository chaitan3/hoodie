import webrtcvad
import sounddevice as sd
import numpy as np

vad_frame_time = 0.03 # ms

class VoiceActivityDetector(object):
    def __init__(self, recorder):
        self.vad = webrtcvad.Vad()
        self.recorder = recorder

    def wait_inactive(self, min_time=0):
        audio = []
        total_frames = 0
        min_frames = int(min_time * self.recorder.sample_rate)
        frames = int(vad_frame_time * self.recorder.sample_rate)
        def callback(indata, frame_count, time, status):
            nonlocal total_frames, audio
            audio.append(indata.copy())
            total_frames += frames
            if total_frames > min_frames:
                res = self.vad.is_speech(indata.tobytes(), self.recorder.sample_rate)
                if res:
                    raise sd.CallbackStop()
        self.recorder.read_callback(callback, frames)
        return np.concatenate(audio).flatten()
