from transformers import WhisperProcessor, WhisperForConditionalGeneration
from datasets import load_dataset
import librosa

# load model and processor
processor = WhisperProcessor.from_pretrained("openai/whisper-small.en")
model = WhisperForConditionalGeneration.from_pretrained("openai/whisper-small.en")
model_samp_rate = 16000

def get_text_from_audio(audio_data, samp_rate):
    re_audio_data = librosa.resample(audio_data, orig_sr=samp_rate, target_sr=model_samp_rate)
    input_features = processor(re_audio_data, sampling_rate=model_samp_rate, return_tensors="pt").input_features 
    predicted_ids = model.generate(input_features)
    transcription = processor.batch_decode(predicted_ids, skip_special_tokens=True)
    return transcription[0]

if __name__ == "__main__":
    import librosa
    audio, _ = librosa.load("audiofile10Seconds.wav", sr=model_samp_rate)
    print(get_text_from_audio(audio))
