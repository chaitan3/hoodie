from speechbrain.pretrained import Tacotron2
from speechbrain.pretrained import HIFIGAN
from scipy.ndimage import zoom

# Intialize TTS (tacotron2) and Vocoder (HiFIGAN)
tacotron2 = Tacotron2.from_hparams(source="speechbrain/tts-tacotron2-ljspeech", savedir="tmpdir_tts")
hifi_gan = HIFIGAN.from_hparams(source="speechbrain/tts-hifigan-ljspeech", savedir="tmpdir_vocoder")

def get_audio_from_text(text):
    mel_output, mel_length, alignment = tacotron2.encode_text(text)
    waveforms = hifi_gan.decode_batch(mel_output)
    audio = waveforms.squeeze()
    samp_rate = 22050
    return zoom(audio, 2), samp_rate*2

if __name__ == "__main__":
    import soundfile as sf
    audio, rate = get_text_from_audio("What's up buddy?")
    sf.write("test.wav", audio, rate, format="WAV")
