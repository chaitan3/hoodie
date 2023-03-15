from flask import Flask
from flask import request
from flask import jsonify, send_file
import soundfile as sf
import time

from asr import get_text_from_audio, model_samp_rate
from intent import parse_text
from actions import perform_task, save_audio_from_text
from logger import logging

app = Flask(__name__)

@app.route('/asr', methods=['POST'])
def handler_asr():
    audio_file = request.files.get("audio")
    audio, samp_rate = sf.read(audio_file)
    text = get_text_from_audio(audio, samp_rate)
    return jsonify({"text": text})

@app.route('/tts', methods=['POST', "GET"])
def handler_tts():
    text = request.values.get("text")
    response, mimetype = save_audio_from_text(text)
    return send_file(response, mimetype=mimetype)

@app.route('/task', methods=['POST', "GET"])
def handler_task():
    try:
        text = request.values.get("text")
        intent = parse_text(text)
    except KeyError:
        intent = request.values.get("intent")
    response = perform_task(intent)
    if isinstance(response, dict):
        return jsonify(response)
    else:
        return send_file(response, mimetype="audio/ogg")

@app.route('/query', methods=['POST'])
def handler_query():
    audio_file = request.files.get("audio")
    t1 = time.time()
    audio, samp_rate = sf.read(audio_file)
    t2 = time.time()
    text = get_text_from_audio(audio, samp_rate)
    t3 = time.time()
    intent = parse_text(text)
    t4 = time.time()
    response = perform_task(intent)
    t5 = time.time()
    logging.info("Timings:")
    logging.info(("Read audio", t2-t1))
    logging.info(("Transcribe", t3-t2))
    logging.info(("Parse", t4-t3))
    logging.info(("Perform", t5-t4))
    if isinstance(response, dict):
        return jsonify(response)
    else:
        return send_file(response, mimetype="audio/ogg")

if __name__ == "__main__":
    app.run("0.0.0.0")
