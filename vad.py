import webrtcvad

vad_frame_time = 0.03 # ms

class VoiceActivityDetector(object):
    def __init__(self):
        self.vad = webrtcvad.Vad()

    def detect(self, recorder):
        audio = recorder.read(frame_time=vad_frame_time)
        res = self.vad.is_speech(audio.tobytes(), recorder.sample_rate)
        if res:
            return audio
        return None
