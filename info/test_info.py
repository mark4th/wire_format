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

def compile_artifacts(text):
    with tempfile.TemporaryDirectory(prefix='wfc-info-') as path:
        stem = Path(path) / 'protocol'
        stem.with_suffix('.wf').write_text(text)
        result = subprocess.run([str(Path(__file__).resolve().parents[1] / 'wfc'),
            str(stem.with_suffix('.wf')), '--output', str(stem)], capture_output=True, text=True)
        if result.returncode:
            raise ValueError(result.stderr)
        return stem.with_suffix('.h').read_text(), stem.with_suffix('.wi').read_bytes()

def compile_text(text):
    return compile_artifacts(text)[1]

def compile_source(source):
    return compile_text(json.dumps(source))

def wi_crc32c(data):
    crc = 0xffffffff
    for index, stored in enumerate(data):
        byte = 0 if 60 <= index < 64 else stored
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
    return crc ^ 0xffffffff

def set_wi_crc32c(data):
    result = bytearray(data)
    struct.pack_into('<I', result, 60, wi_crc32c(result))
    return bytes(result)

assert wi_crc32c(b'123456789') == 0xe3069283

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
generated_header, image = compile_artifacts(json.dumps(source))
assert '#define TEST_WI_FORMAT_VERSION 4u' in generated_header
assert '#define TEST_WI_FILE_SIZE ' in generated_header
assert '#define TEST_WI_CRC32C ' in generated_header
assert '#define TEST_MESSAGE_RECORD 0u' in generated_header
assert '#define TEST_RECORD_FIELD_COUNT 6u' in generated_header
assert '#define TEST_RECORD_ORDINAL_PAYLOAD 4u' in generated_header
assert '#define TEST_RECORD_TYPE_PAYLOAD 2u' in generated_header
with tempfile.TemporaryDirectory(prefix='wfc-header-') as path:
    path = Path(path)
    (path / 'test.h').write_text(generated_header)
    (path / 'consumer.c').write_text(
        '#include "test.h"\n'
        '_Static_assert(TEST_MESSAGE_RECORD == 0u, "message");\n'
        '_Static_assert(TEST_RECORD_ORDINAL_PAYLOAD == 4u, "field");\n'
        'int main(void) { return 0; }\n')
    result = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-fsyntax-only', str(path / 'consumer.c')], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr

nested = {'wire_format': 4, 'protocol': {'name': 'nested', 'messages': [
    {'name': 'item', 'fields': [{'name': 'value', 'type': 'uint', 'width': 8}]},
    {'name': 'batch', 'fields': [
        {'name': 'items', 'type': 'records', 'message': 'item', 'count': 1}]}
]}}
nested_header, _ = compile_artifacts(json.dumps(nested))
assert '#define NESTED_MESSAGE_ITEM 0u' in nested_header
assert '#define NESTED_MESSAGE_BATCH 1u' in nested_header
assert '#define NESTED_BATCH_CHILD_ITEMS NESTED_MESSAGE_ITEM' in nested_header
assert struct.unpack_from('<I', image, 60)[0] == wi_crc32c(image)
r = Runtime(args.library, image)
r.lib.wi_file_crc32c.argtypes = [C.c_void_p, C.c_size_t]
r.lib.wi_file_crc32c.restype = C.c_uint32
check_text = C.create_string_buffer(b'123456789')
assert r.lib.wi_file_crc32c(check_text, 9) == 0xe3069283
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
changed = set_wi_crc32c(changed)
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
    return set_wi_crc32c(changed)
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
    buf = C.create_string_buffer(set_wi_crc32c(bad))
    db = Database()
    assert r.lib.wire_info_open(C.byref(db), buf, len(bad)) != 0
    assert not db.data
# Syntactically valid stack underflow must return an error, never abort.
bad = bytearray(image)
start = bad.index(b'%{10}')
bad[start:start+3] = b'%i\0'
r_bad = Runtime(args.library, set_wi_crc32c(bad))
record = r_bad.record(0, {})
assert r_bad.lib.wire_info_encode(C.byref(r_bad.db), 0, C.byref(record), out, len(out), C.byref(used)) != 0
assert used.value == 0
# Corruption in each populated file section, or in the stored checksum itself,
# is rejected specifically as a checksum failure before contents are exposed.
message_table = struct.unpack_from('<I', image, 24)[0]
metadata = struct.unpack_from('<I', image, 28)[0]
offsets = struct.unpack_from('<I', image, 44)[0]
strings = struct.unpack_from('<I', image, 52)[0]
for position in (32, message_table, metadata, offsets, strings, 60):
    corrupt = bytearray(image)
    corrupt[position] ^= 1
    buf = C.create_string_buffer(bytes(corrupt))
    db = Database()
    assert r.lib.wire_info_open(C.byref(db), buf, len(corrupt)) == 7
    assert not db.data
# A release-0.1.0-style image with no integrity flag or checksum is rejected
# and therefore cannot be mistaken for a current catalog.
legacy = bytearray(image)
struct.pack_into('<I', legacy, 12, 0)
struct.pack_into('<I', legacy, 60, 0)
buf = C.create_string_buffer(bytes(legacy))
db = Database()
assert r.lib.wire_info_open(C.byref(db), buf, len(legacy)) == 2
assert not db.data
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
    schema([{'name': 'x-y', 'type': 'uint', 'width': 8},
            {'name': 'x_y', 'type': 'uint', 'width': 8}]),
    {'wire_format': 4, 'protocol': {'name': 'test', 'messages': [
        {'name': 'x-y', 'fields': []}, {'name': 'x_y', 'fields': []}]}},
    {'wire_format': 4, 'protocol': {'name': '1bad', 'messages': [
        {'name': 'record', 'fields': []}]}},
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
