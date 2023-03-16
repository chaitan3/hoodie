from flask import Flask
from flask import request
from flask import jsonify, send_file, abort
import soundfile as sf
import time
import os

from asr import get_text_from_audio, model_samp_rate
from intent import parse_text
from actions import perform_task, save_audio_from_text
from actions import runner, audio_mimetype, sleep_time
from logger import logging

app = Flask(__name__)

@app.route('/audio', methods=['POST', 'GET'])
def handler_audio():
    audio_file = runner.get_cache(request.values.get("file"))
    start_time = time.time()
    max_wait_time = 5
    while (audio_file not in runner.cache) and (time.time() - start_time) < max_wait_time:
        time.sleep(sleep_time / 1000)
    return send_file(audio_file, mimetype=audio_mimetype)

@app.route('/asr', methods=['POST'])
def handler_asr():
    audio_file = request.files.get("audio")
    audio, samp_rate = sf.read(audio_file)
    text = get_text_from_audio(audio, samp_rate)
    return jsonify({"text": text})

@app.route('/tts', methods=['POST', "GET"])
def handler_tts():
    text = request.values.get("text")
    audio_file = BytesIO()
    save_audio_from_text(audio_file, text)
    audio_file.seek(0)
    return send_file(audio_file, mimetype=audio_mimetype)

@app.route('/task', methods=['POST', "GET"])
def handler_task():
    try:
        text = request.values.get("text")
        intent = parse_text(text)
    except KeyError:
        intent = request.values.get("intent")
    output = perform_task(intent)
    return jsonify({"intent": intent, "output": output})

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
    output = perform_task(intent)
    t5 = time.time()
    logging.info("Timings:")
    logging.info(("Read audio", t2-t1))
    logging.info(("Transcribe", t3-t2))
    logging.info(("Parse", t4-t3))
    logging.info(("Perform", t5-t4))
    response = {
            "text": text,
            "intent": intent,
            "output": output
    }
    return jsonify(response)

if __name__ == "__main__":
    try:
        runner.start()
        app.run("0.0.0.0", threaded=True)
    except KeyboardInterrupt:
        runner.stop()
        runner.join()
