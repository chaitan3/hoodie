from transformers import WhisperProcessor, WhisperForConditionalGeneration
from datasets import load_dataset

# load model and processor
processor = WhisperProcessor.from_pretrained("openai/whisper-tiny.en")
model = WhisperForConditionalGeneration.from_pretrained("openai/whisper-tiny.en")
model_samp_rate = 16000

def get_text_from_audio(audio_data):
    import librosa
    input_features = processor(audio_data, sampling_rate=model_samp_rate, return_tensors="pt").input_features 
    predicted_ids = model.generate(input_features)
    transcription = processor.batch_decode(predicted_ids, skip_special_tokens=True)
    return transcription[0]

if __name__ == "__main__":
    import librosa
    audio, _ = librosa.load("audiofile10Seconds.wav", sr=model_samp_rate)
    print(get_text_from_audio(audio))
