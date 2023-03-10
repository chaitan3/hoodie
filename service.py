from flask import Flask
from flask import request
from flask import jsonify, send_file
import soundfile as sf

from asr import get_text_from_audio, model_samp_rate
from intent import parse_text
from actions import perform_task

app = Flask(__name__)

@app.route('/receive_and_respond', methods=['POST'])
def handler():
    audio_file = request.files["audio"]
    audio, samp_rate = sf.read(audio_file)
    resp = {}
    text = get_text_from_audio(audio, samp_rate)
    print('text', text)
    intent = parse_text(text)
    print('intent', intent)
    response = perform_task(intent)
    return send_file(response, download_name="response.wav")

if __name__ == "__main__":
    app.run()
