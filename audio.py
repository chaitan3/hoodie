import pyaudio
import numpy as np
import soundfile as sf
from io import BytesIO

fmt = pyaudio.paFloat32
chunk_size = 1024

input_device_name = "default:CARD=camera"
input_channels = 1
input_device_id = 9
input_samp_rate = 44100

output_device_id = 10
output_samp_rate = 44100

p = pyaudio.PyAudio()

def record_audio(samp_size, input_stream):
    frames = []
    for i in range(0, (input_samp_rate//chunk_size) * samp_size):
        data = input_stream.read(chunk_size, exception_on_overflow=False)
        frames.append(np.frombuffer(data, dtype=np.float32))
    audio = np.concatenate(frames)
    return audio, input_samp_rate

def play_audio(audio, output_stream):
    for i in range(0, len(audio), chunk_size):
        data = audio[i:i+chunk_size].astype(np.float32).tobytes()
        output_stream.write(data)

def record_audio_to_bytes(samp_size=3):
    assert p.is_format_supported(rate=input_samp_rate,
                                 input_device=input_device_id,
                                 input_channels=input_channels,
                                 input_format=fmt)
    input_stream = p.open(format=fmt, rate=input_samp_rate, \
                    channels=input_channels, \
                    input_device_index=input_device_id, input=True, \
                    frames_per_buffer=chunk_size)
    audio_file = BytesIO()
    audio, wav_rate = record_audio(samp_size, input_stream)
    sf.write(audio_file, audio, wav_rate, format="WAV")
    audio_file.seek(0)
    input_stream.close()
    return audio_file

def play_audio_from_bytes(audio_bytes):
    audio_file = BytesIO(audio_bytes)
    audio, wav_rate = sf.read(audio_file)
    audio = audio.astype(np.float32)
    output_channels = 1
    if len(audio.shape) > 1:
        output_channels = audio.shape[1]
    rate = wav_rate
    try:
        p.is_format_supported(rate=rate,
                             output_device=output_device_id,
                             output_channels=output_channels,
                             output_format=fmt)
    except ValueError:
        import librosa
        audio = librosa.resample(audio, orig_sr=wav_rate, target_sr=output_samp_rate)
        rate = output_samp_rate
    assert p.is_format_supported(rate=rate,
                                 output_device=output_device_id,
                                 output_channels=output_channels,
                                 output_format=fmt)
    output_stream = p.open(format=fmt, rate=rate, \
                    channels=output_channels, \
                    output_device_index=output_device_id, output=True, \
                    frames_per_buffer=chunk_size)
    play_audio(audio, output_stream)
    output_stream.close()

if __name__ == "__main__":
    for i in range(p.get_device_count()):
        print (p.get_device_info_by_index(i))
    from wakeword import wait_for_wakeword
    wait_for_wakeword()
    record_audio_to_bytes()
    wait_for_wakeword()
    record_audio_to_bytes()

    #import librosa
    #audio = librosa.resample(record_audio(3), orig_sr=device_samp_rate, target_sr=16000)
    #soundfile.write("test.wav", audio, 16000)
    #from whisper import get_text_from_audio
    #print(get_text_from_audio(audio))

    #audio, wav_rate = librosa.load("audiofile10Seconds.wav")
    #audio = audio[:,0]
    #audio = librosa.resample(audio, orig_sr=wav_rate, target_sr=device_samp_rate)
    #play_audio(audio)
