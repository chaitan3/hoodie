import numpy as np
import soundfile as sf
import sounddevice as sd
import time

dtype = np.float32

# desktop
#input_device_name = "default:CARD=camera"
#input_device_id = 10
#output_device_id = 11
#blocksize = 0

# raspi
input_device_name = "sysdefault:CARD=Device"
input_device_id = 1
output_device_id = 0
blocksize = 1024

def record_audio_to_file(file_obj, samp_size=3):
    input_channels = 1
    input_samp_rate = 44100

    audio = sd.rec(device=input_device_id,
                   frames=samp_size * input_samp_rate,
                   samplerate=input_samp_rate,
                   channels=input_channels,
                   dtype=dtype)
    sd.wait()
    sf.write(file_obj, audio, input_samp_rate, format="WAV")
    if hasattr(file_obj, "seek"):
        file_obj.seek(0)

def play_audio_from_file(file_obj):
    with sf.SoundFile(file_obj) as audio_file:
        dtype = np.float32
        def callback(outdata, frame_count, time, status):
            data = audio_file.read(frame_count, dtype=dtype, out=outdata)
            if (data.size != outdata.size):
                outdata[data.shape[0]:,:].fill(0.0)
                raise sd.CallbackStop()
        with sd.OutputStream(device=output_device_id, samplerate=audio_file.samplerate,
                           channels=audio_file.channels,
                           dtype=dtype,
                           callback=callback,
                           blocksize=blocksize) as ss:
            while ss.active:
               sd.sleep(int(0.1*1000))

if __name__ == "__main__":
    print(sd.query_devices())
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

    play_audio_from_file('test3.ogg')
    #record_audio_to_file("sd.wav")

    #audio, wav_rate = librosa.load("audiofile10Seconds.wav")
    #audio = audio[:,0]
    #audio = librosa.resample(audio, orig_sr=wav_rate, target_sr=device_samp_rate)
    #play_audio(audio)
