import pyaudio
import numpy as np
import soundfile as sf
import time

fmt = pyaudio.paFloat32
chunk_size = 32768

#input_device_name = "default:CARD=camera"
input_device_name = "sysdefault:CARD=Device"
input_channels = 1
#input_device_id = 10
input_device_id = 1
input_samp_rate = 44100

#output_device_id = 11
output_device_id = 0
output_samp_rate = 44100

p = pyaudio.PyAudio()

def record_audio_to_file(file_obj, samp_size=3):
    assert p.is_format_supported(rate=input_samp_rate,
                                 input_device=input_device_id,
                                 input_channels=input_channels,
                                 input_format=fmt)
    input_stream = p.open(format=fmt, rate=input_samp_rate, \
                    channels=input_channels, \
                    input_device_index=input_device_id, input=True, \
                    frames_per_buffer=chunk_size)
    frames = []
    for offset in np.arange(0, input_samp_rate  * samp_size, chunk_size):
        data = input_stream.read(chunk_size, exception_on_overflow=False)
        frames.append(np.frombuffer(data, dtype=np.float32))
    audio = np.concatenate(frames)
    sf.write(file_obj, audio, input_samp_rate, format="WAV")
    file_obj.seek(0)
    input_stream.close()

def play_audio_from_file(file_obj):
    audio_file = sf.SoundFile(file_obj)
    wav_rate = audio_file.samplerate
    output_channels = audio_file.channels
    assert p.is_format_supported(rate=wav_rate,
                                 output_device=output_device_id,
                                 output_channels=output_channels,
                                 output_format=fmt)
    def callback(in_data, frame_count, time_info, status):
        data = audio_file.read(frames=frame_count, dtype=np.float32)
        return (data, pyaudio.paContinue)
    output_stream = p.open(format=fmt, rate=wav_rate, \
                    channels=output_channels, \
                    output_device_index=output_device_id, output=True,
                    stream_callback=callback,
                    frames_per_buffer=chunk_size)
    while output_stream.is_active():
        time.sleep(0.1)
    output_stream.stop_stream()
    output_stream.close()

if __name__ == "__main__":
    for i in range(p.get_device_count()):
        print (p.get_device_info_by_index(i))
    #from wakeword import wait_for_wakeword
    #wait_for_wakeword()
    #record_audio_to_bytes()
    #wait_for_wakeword()
    #record_audio_to_bytes()

    #import librosa
    #audio = librosa.resample(record_audio(3), orig_sr=device_samp_rate, target_sr=16000)
    #soundfile.write("test.wav", audio, 16000)
    #from whisper import get_text_from_audio
    #print(get_text_from_audio(audio))

    f = 'test.ogg'
    play_audio_from_file(f)

    #audio, wav_rate = librosa.load("audiofile10Seconds.wav")
    #audio = audio[:,0]
    #audio = librosa.resample(audio, orig_sr=wav_rate, target_sr=device_samp_rate)
    #play_audio(audio)
