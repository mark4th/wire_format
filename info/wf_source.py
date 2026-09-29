"""Read the JSON-with-hex subset of .wf used by the Python example tools.

wfc accepts JSON5. These tools accept JSON plus hexadecimal integer literals;
normalizing only unquoted hex tokens keeps strings and integer precision intact.
"""
import json
from pathlib import Path
import re


_TOKENS = re.compile(r'"(?:[^"\\]|\\.)*"|(?<![\w.])0[xX][0-9a-fA-F]+(?![\w.])')


def loads_source(text):
    def normalize(match):
        token = match.group(0)
        return token if token.startswith('"') else str(int(token, 16))

    return json.loads(_TOKENS.sub(normalize, text))


def load_source(path):
    return loads_source(Path(path).read_text())
