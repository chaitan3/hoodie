from audio import input_device_name, input_channels

import subprocess
import sys

model_samp_rate = 16000
model_fmt = "S16_LE"

def wait_for_wakeword():
    recorder = f"arecord -r {model_samp_rate} -f {model_fmt} -c {input_channels} -t raw -D {input_device_name}"
    detector = "rhasspy-wake-raven --keyword raven/keyword/ probability-threshold=0.5 --exit-count 1"
    subprocess.check_call(["bash", "-c", f"{recorder} | {detector}"])

if __name__ == "__main__":
    wait_for_wakeword()
