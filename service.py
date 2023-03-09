from flask import Flask
from flask import request
from flask import jsonify, send_file

import librosa
from whisper import get_text_from_audio, model_samp_rate
from intent import parse_text
from actions import perform_task

app = Flask(__name__)

@app.route('/receive_and_respond', methods=['POST'])
def handler():
    audio_file = request.files["audio"]
    audio, _ = librosa.load(audio_file, sr=model_samp_rate)
    resp = {}
    text = get_text_from_audio(audio)
    print('text', text)
    intent = parse_text(text)
    print('intent', intent)
    response = perform_task(intent)
    return send_file(response)

if __name__ == "__main__":
    app.run()
