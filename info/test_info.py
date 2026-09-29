#!/usr/bin/env python3
"""Compiler rejection and runtime boundary tests, independent of any protocol."""
import argparse
import copy
import ctypes as C
import json
import subprocess
import tempfile
from pathlib import Path
import struct
from vectors import Runtime, Database
from wf_source import loads_source

def compile_text(text):
    with tempfile.TemporaryDirectory(prefix='wfc-info-') as path:
        stem = Path(path) / 'protocol'
        stem.with_suffix('.wf').write_text(text)
        result = subprocess.run([str(Path(__file__).resolve().parents[1] / 'wfc'),
            str(stem.with_suffix('.wf')), '--output', str(stem)], capture_output=True, text=True)
        if result.returncode:
            raise ValueError(result.stderr)
        return stem.with_suffix('.wi').read_bytes()

def compile_source(source):
    return compile_text(json.dumps(source))

# Hex numbers retain all 64 bits; hex-looking strings and escaped quotes stay
# untouched. Invalid numeric tokens must still be rejected.
assert loads_source(r'{"max":0xFFFFFFFFFFFFFFFF,"bytes":[0x00,0Xff],"label":"0xFF","quoted":"\"0x10\"","path":"C:\\0xAB"}') == {
    'max': (1 << 64) - 1, 'bytes': [0, 255], 'label': '0xFF',
    'quoted': '"0x10"', 'path': 'C:\\0xAB'}
for token in ('0x', '0xGG', '0x12g', '00x12', '0x1.2', '1e0x2'):
    try:
        loads_source('{"value":' + token + '}')
    except ValueError:
        pass
    else:
        raise AssertionError(f'accepted invalid hex literal: {token}')

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--library', required=True)
args = p.parse_args()

def schema(fields):
    return {'wire_format': 4, 'protocol': {'name': 'test', 'messages': [{'name': 'record', 'fields': fields}]}}

source = schema([
    {'name': 'constant', 'type': 'uint', 'width': 4, 'constant': 10},
    {'name': 'small', 'type': 'uint', 'width': 4},
    {'name': 'little', 'type': 'uint', 'width': 16, 'byte_order': 'little-endian'},
    {'name': 'size', 'type': 'sdnv', 'value': ['len', 'payload']},
    {'name': 'payload', 'type': 'bytes', 'length': 'size'},
    {'name': 'crc', 'type': 'crc'}])
image = compile_source(source)
r = Runtime(args.library, image)
# Published CCITT-FALSE check value for ASCII "123456789".
c = Runtime(args.library, compile_source(schema([
    {'name': 'data', 'type': 'bytes', 'length': 9}, {'name': 'crc', 'type': 'crc'}])))
c.vector(0, {'values': {'data': '123456789'}, 'wire': '31323334353637383929b1', 'strict_prefixes': True})
# Standard reflected CRC check values for the same ASCII input.
for algorithm, expected in [(2, '313233343536373839906e'), (3, '313233343536373839e3069283')]:
    c = Runtime(args.library, compile_source(schema([
        {'name': 'data', 'type': 'bytes', 'length': 9}, {'name': 'crc', 'type': 'crc', 'algorithm': algorithm}])))
    c.vector(0, {'values': {'data': '123456789'}, 'wire': expected, 'strict_prefixes': True})

for length in [0, 1, 127, 128, 255, 1024]:
    payload = bytes(i & 255 for i in range(length))
    source_values = {'small': 15, 'little': 0x1234, 'payload': list(payload)}
    record = r.record(0, source_values)
    output = C.create_string_buffer(length + 32)
    used = C.c_size_t()
    r.check(r.lib.wire_info_encode(C.byref(r.db), 0, C.byref(record), output, len(output), C.byref(used)))
    assert output.raw[:3] == b'\xaf\x34\x12'
    encoded_length = used.value
    decoded = r.record(0, {}, True)
    r.check(r.lib.wire_info_decode(C.byref(r.db), 0, C.byref(decoded), output, encoded_length, C.byref(used)))
    assert r.values(0, decoded)['payload'] == list(payload)
    for capacity in range(encoded_length):
        used.value = 999
        assert r.lib.wire_info_encode(C.byref(r.db), 0, C.byref(record), output, capacity, C.byref(used)) != 0
        assert used.value == 0
    r.keep.clear()

# The .wi table contains live format strings. Changing one changes the wire
# without recompiling the library or the application.
assert image[:8] == b'WI\0\0\4\0\x40\0'
assert b'%{10}%P{constant}' in image
changed = image.replace(b'%{10}%P{constant}', b'%{11}%P{constant}', 1)
r_changed = Runtime(args.library, changed)
record = r_changed.record(0, {'small': 15, 'payload': []})
out, used = C.create_string_buffer(64), C.c_size_t()
r_changed.check(r_changed.lib.wire_info_encode(C.byref(r_changed.db), 0,
    C.byref(record), out, len(out), C.byref(used)))
assert out.raw[0] == 0xbf
r.lib.wire_info_format.argtypes = [C.POINTER(Database), C.c_uint, C.c_int]
r.lib.wire_info_format.restype = C.c_char_p
assert r.lib.wire_info_format(C.byref(r.db), 0, 0).startswith(b'%{10}')
# Adding a table entry needs only source compilation; the same loaded library
# handles the new message and full-width unsigned arithmetic.
added = copy.deepcopy(source)
added['protocol']['messages'].append({'name': 'new-entry', 'fields': [
    {'name': 'high', 'type': 'uint', 'width': 64, 'constant': (1 << 64) - 1},
    {'name': 'compare', 'type': 'uint', 'width': 8, 'value': ['>', 'high', 1 << 63]}]})
r_added = Runtime(args.library, compile_source(added))
r_added.vector(1, {'values': {}, 'wire': 'ffffffffffffffff01', 'strict_prefixes': True})
hex_text = json.dumps(added).replace(str((1 << 64) - 1), '0xFFFFFFFFFFFFFFFF')
assert loads_source(hex_text) == added
assert compile_text(hex_text) == compile_source(added)
# Nested conditionals must skip whole branches, including nested else clauses.
def program_image(program):
    changed = bytearray(image)
    messages = struct.unpack_from('<I', changed, 24)[0]
    section = struct.unpack_from('<I', changed, messages)[0]
    strings = struct.unpack_from('<I', changed, 52)[0]
    struct.pack_into('<I', changed, section + 8, len(changed) - strings)
    changed.extend(program + b'\0')
    struct.pack_into('<I', changed, 8, len(changed))
    struct.pack_into('<I', changed, 56, len(changed) - strings)
    return bytes(changed)
for condition, expected in [(0, 0x33), (1, 0x22)]:
    program = (b'%?%{' + str(condition).encode() + b'}%t%?%{0}%t%{17}%{8}%i'
               b'%e%{34}%{8}%i%;%e%{51}%{8}%i%;%uF')
    r_nested = Runtime(args.library, program_image(program))
    record = r_nested.record(0, {})
    r_nested.check(r_nested.lib.wire_info_encode(C.byref(r_nested.db), 0,
        C.byref(record), out, len(out), C.byref(used)))
    assert used.value == 1 and out.raw[0] == expected
# Stack overflow and runtime arithmetic failures are bounded errors.
for program in [b'%{0}' * 65, b'%{0}%{1}%u-', b'%{1}%{0}%u/', b'%{1}%{64}%u[']:
    r_bad = Runtime(args.library, program_image(program))
    record = r_bad.record(0, {})
    assert r_bad.lib.wire_info_encode(C.byref(r_bad.db), 0,
        C.byref(record), out, len(out), C.byref(used)) != 0
    assert used.value == 0
# Malformed format syntax and unsafe pointer opcodes are rejected at open.
for replacement in (b'%r', b'%?', b'%u', b'%J', b'%g', b'%{'):
    bad = bytearray(image)
    start = bad.index(b'%{10}')
    bad[start:start+3] = replacement + b'\0'
    buf = C.create_string_buffer(bytes(bad))
    db = Database()
    assert r.lib.wire_info_open(C.byref(db), buf, len(bad)) != 0
    assert not db.data
# Syntactically valid stack underflow must return an error, never abort.
bad = bytearray(image)
start = bad.index(b'%{10}')
bad[start:start+3] = b'%i\0'
r_bad = Runtime(args.library, bytes(bad))
record = r_bad.record(0, {})
assert r_bad.lib.wire_info_encode(C.byref(r_bad.db), 0, C.byref(record), out, len(out), C.byref(used)) != 0
assert used.value == 0
for size in range(len(image)):
    db = Database()
    assert r.lib.wire_info_open(C.byref(db), r.image, size) != 0

invalid = [
    {"wire_format": 4, "protocol": "bad"},
    schema([{"name": "bad%name", "type": "uint", "width": 8}]),
    schema([{'name': 'x', 'type': 'uint', 'width': 'typo'}]),
    schema([{'name': 'x', 'type': 'uint', 'width': 8, 'length': 1}]),
    schema([{'name': 'x', 'type': 'records', 'message': 'record'}]),
    schema([{'name': 'x', 'type': 'bytes', 'length': 1, 'decode_length': 2}]),
    schema([{'name': 'x', 'type': 'crc', 'algorithm': ['bogus', 1]}]),
    schema([{'name': 'x', 'type': 'uint', 'width': 8, 'constant': 1, 'value': 2}]),
    schema([{'name': 'x', 'type': 'bytes'}, {'name': 'x', 'type': 'bytes'}]),
]
for bad in invalid:
    try:
        compile_source(bad)
    except ValueError:
        pass
    else:
        raise AssertionError(f'accepted invalid schema: {bad}')
with tempfile.TemporaryDirectory(prefix='wfc-duplicate-') as path:
    wf = Path(path) / 'duplicate.wf'
    wf.write_text('{"wire_format":4,"wire_format":4,"protocol":{}}')
    assert subprocess.run([str(Path(__file__).resolve().parents[1] / 'wfc'), 'check', str(wf)],
        capture_output=True).returncode != 0
# Deterministic compiler output.
assert image == compile_source(copy.deepcopy(source))
print('wire_info: compiler rejection, format-string execution/validation, CRC check values, endian and buffer boundary tests passed')
