import string
import regex as re

from logger import logging
from config import wake_keyword

def remove_punctuation(text):
    return re.sub(r"\p{P}+", "", text)

def tokenize(text):
    return text.strip().lower().split(' ')

def parse_text(text):
    tokens = tokenize(text)
    cmd = ""
    args = []
    intent = {}
    valid = False
    if len(tokens) > 0:
        key = remove_punctuation(tokens[0])
        if key != wake_keyword:
            cmd = "false_positive"
        elif len(tokens) > 1:
            cmd = remove_punctuation(tokens[1])
        if len(tokens) > 2:
            args = tokens[2:]
    intent["command"] = cmd
    intent["arguments"] = args
    logging.info(("Query response:", str(intent)))
    return intent

if __name__ == "__main__":
    text = f"{wake_keyword} play bon jovi its my life"
    parse_text(text)
