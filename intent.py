import string
import regex as re

from logger import logging

def remove_punctuation(text):
    return re.sub(r"\p{P}+", "", text)

def tokenize(text):
    return text.strip().lower().split(' ')

def parse_text(text):
    tokens = tokenize(text)
    cmd = ""
    args = []
    if len(tokens) > 0:
        cmd = remove_punctuation(tokens[0])
    if len(tokens) > 1:
        args = tokens[1:]
    intent = {}
    intent["command"] = cmd
    intent["arguments"] = args
    logging.info(("Intent:", intent))
    return intent

if __name__ == "__main__":
    text = "play bon jovi its my life"
    parse_text(text)
