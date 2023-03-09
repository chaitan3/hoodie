import subprocess

def search_and_download_audio(key):
    fmt = "wav"
    name = f"tmp_audio.{fmt}"
    cmd = f'yt-dlp -f "ba" -x --audio-format {fmt} ytsearch1:"{key}" -o {name}'
    subprocess.check_call(cmd, shell=True)
    return name

def perform_task(intent):
    cmd = intent["command"]
    args = intent["arguments"]
    if cmd == "play":
        print("playing", args)
        return search_and_download_audio(" ".join(args))
    elif cmd == "stop":
        pass
    else:
        pass
