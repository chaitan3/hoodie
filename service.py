from flask import Flask
from flask import request
from flask import jsonify, send_file
import soundfile as sf
import time

from asr import get_text_from_audio, model_samp_rate
from intent import parse_text
from actions import perform_task

app = Flask(__name__)

@app.route('/receive_and_respond', methods=['POST'])
def handler():
    audio_file = request.files["audio"]
    t1 = time.time()
    audio, samp_rate = sf.read(audio_file)
    resp = {}
    t2 = time.time()
    text = get_text_from_audio(audio, samp_rate)
    print('TEXT:', text)
    t3 = time.time()
    intent = parse_text(text)
    print('INTENT:', intent)
    t4 = time.time()
    response = perform_task(intent)
    t5 = time.time()
    print("Timings:")
    print("\tRead audio", t2-t1)
    print("\tTranscribe", t3-t2)
    print("\tParse", t4-t3)
    print("\tPerform", t5-t4)
    return send_file(response, download_name="response.wav")

if __name__ == "__main__":
    app.run("0.0.0.0")
