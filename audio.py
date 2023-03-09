import pyaudio
import numpy as np
import soundfile as sf
import librosa
from io import BytesIO

form_1 = pyaudio.paFloat32
chans = 1
device_samp_rate = 44100
chunk_size = 1024
input_device = 10
output_device = 11

p = pyaudio.PyAudio()
for i in range(p.get_device_count()):
    print (p.get_device_info_by_index(i))

input_stream = p.open(format=form_1, rate=device_samp_rate,
                    channels=chans, \
                    input_device_index=input_device, input=True, \
                    frames_per_buffer=chunk_size)


def record_audio(samp_size):
    frames = []
    for i in range(0, (device_samp_rate//chunk_size) * samp_size):
        data = input_stream.read(chunk_size, exception_on_overflow=False)
        frames.append(np.frombuffer(data, dtype=np.float32))
    audio = np.concatenate(frames)
    return audio

def play_audio(audio, output_stream):
    for i in range(0, len(audio), chunk_size):
        data = audio[i:i+chunk_size].astype(np.float32).tobytes()
        output_stream.write(data)

def record_audio_to_bytes():
    audio_file = BytesIO()
    sf.write(audio_file, record_audio(3), device_samp_rate, format="WAV")
    audio_file.seek(0)
    return audio_file

def play_audio_from_bytes(audio):
    audio_file = BytesIO(audio)
    audio, wav_rate = sf.read(audio_file)
    output_stream = p.open(format=form_1, rate=wav_rate,
                    channels=audio.shape[1], \
                    output_device_index=output_device, output=True,
                    frames_per_buffer=chunk_size)
    play_audio(audio, output_stream)

if __name__ == "__main__":
    import librosa
    #audio = librosa.resample(record_audio(3), orig_sr=device_samp_rate, target_sr=16000)
    #soundfile.write("test.wav", audio, 16000)
    #from whisper import get_text_from_audio
    #print(get_text_from_audio(audio))
    #input_stream.close()

    audio, wav_rate = librosa.load("audiofile10Seconds.wav")
    audio = audio[:,0]
    audio = librosa.resample(audio, orig_sr=wav_rate, target_sr=device_samp_rate)
    play_audio(audio)
    output_stream.close()
