#!/usr/bin/env python3
"""Run JSON golden vectors through the shared C library (no protocol adapters)."""
import argparse
import ctypes as C
from pathlib import Path
from wf_source import load_source

class Database(C.Structure):
    _fields_ = [('data', C.c_void_p), ('size', C.c_size_t),
                ('messages', C.c_uint32), ('fields', C.c_uint32)]

class Record(C.Structure):
    pass

class Value(C.Structure):
    _fields_ = [('number', C.c_uint64), ('bytes', C.POINTER(C.c_uint8)),
                ('length', C.c_size_t), ('records', C.POINTER(Record)),
                ('count', C.c_size_t), ('capacity', C.c_size_t)]

Record._fields_ = [('values', C.POINTER(Value)), ('capacity', C.c_size_t)]

class Runtime:
    def __init__(self, library, image):
        self.lib = C.CDLL(str(Path(library).resolve()))
        self.lib.wire_info_open.argtypes = [C.POINTER(Database), C.c_void_p, C.c_size_t]
        self.lib.wire_info_find.argtypes = [C.POINTER(Database), C.c_char_p]
        self.lib.wire_info_field_count.argtypes = [C.POINTER(Database), C.c_uint]
        self.lib.wire_info_field_count.restype = C.c_size_t
        for name in ('wire_info_field_name', 'wire_info_field_type', 'wire_info_child'):
            getattr(self.lib, name).argtypes = [C.POINTER(Database), C.c_uint, C.c_uint]
        self.lib.wire_info_field_name.restype = C.c_char_p
        self.lib.wire_info_error.argtypes = [C.c_int]
        self.lib.wire_info_error.restype = C.c_char_p
        for name in ('wire_info_encode', 'wire_info_decode'):
            getattr(self.lib, name).argtypes = [C.POINTER(Database), C.c_uint,
                C.POINTER(Record), C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
        self.image = C.create_string_buffer(image)
        self.db = Database()
        self.check(self.lib.wire_info_open(C.byref(self.db), self.image, len(image)))
        self.keep = []

    def check(self, status):
        if status:
            raise ValueError(self.lib.wire_info_error(status).decode())

    def record(self, message, source, decode=False):
        n = self.lib.wire_info_field_count(C.byref(self.db), message)
        values = (Value * n)()
        record = Record(values, n)
        self.keep.extend((record, values))
        names = set()
        for i in range(n):
            name = self.lib.wire_info_field_name(C.byref(self.db), message, i).decode()
            names.add(name)
            kind = self.lib.wire_info_field_type(C.byref(self.db), message, i)
            item = source.get(name)
            if kind == 4:
                items = item if item is not None else []
                capacity = max(1, len(items))
                child = self.lib.wire_info_child(C.byref(self.db), message, i)
                records = (Record * capacity)()
                for j in range(capacity):
                    records[j] = self.record(child, items[j] if j < len(items) else {}, decode)
                self.keep.append(records)
                values[i].records = records
                values[i].count = 0 if decode else len(items)
                values[i].capacity = capacity
            elif kind in (2, 11, 12):
                if item is not None and not decode:
                    data = item.encode() if isinstance(item, str) else bytes(item)
                    buf = (C.c_uint8 * len(data)).from_buffer_copy(data)
                    self.keep.append(buf)
                    values[i].bytes = buf
                    values[i].length = len(data)
            elif item is not None and (not decode or kind == 5):
                values[i].number = item
        unknown = source.keys() - names
        if unknown:
            raise ValueError(f'unknown vector fields: {sorted(unknown)}')
        return record

    def values(self, message, record):
        result = {}
        for i in range(record.capacity):
            name = self.lib.wire_info_field_name(C.byref(self.db), message, i).decode()
            kind = self.lib.wire_info_field_type(C.byref(self.db), message, i)
            v = record.values[i]
            if kind == 4:
                child = self.lib.wire_info_child(C.byref(self.db), message, i)
                result[name] = [self.values(child, v.records[j]) for j in range(v.count)]
            elif kind in (2, 11, 12):
                result[name] = list(C.string_at(v.bytes, v.length)) if v.length else []
            else:
                result[name] = v.number
        return result

    def vector(self, message, vector):
        source = vector.get('values', {})
        expected = vector['wire']
        expected = bytes.fromhex(expected) if isinstance(expected, str) else bytes(expected)
        encoded_record = self.record(message, source)
        output = C.create_string_buffer(max(1, len(expected) + 32))
        used = C.c_size_t()
        self.check(self.lib.wire_info_encode(C.byref(self.db), message,
            C.byref(encoded_record), output, len(output), C.byref(used)))
        actual = output.raw[:used.value]
        if actual != expected:
            raise ValueError(f'wire mismatch: {actual.hex()} != {expected.hex()}')
        decoded_record = self.record(message, source, True)
        self.check(self.lib.wire_info_decode(C.byref(self.db), message,
            C.byref(decoded_record), output, len(expected), C.byref(used)))
        if used.value != len(expected):
            raise ValueError(f'consumed {used.value} of {len(expected)}')
        before = self.values(message, encoded_record)
        after = self.values(message, decoded_record)
        # Compare supplied fields, including nested records, after generic normalization.
        def compare(a, b, supplied):
            for key in supplied:
                if isinstance(supplied[key], list) and supplied[key] and isinstance(supplied[key][0], dict):
                    if len(a[key]) != len(b[key]): raise ValueError(f'record count mismatch: {key}')
                    for x, y, z in zip(a[key], b[key], supplied[key]): compare(x, y, z)
                elif a[key] != b[key]:
                    raise ValueError(f'decoded {key}: {b[key]!r} != {a[key]!r}')
        compare(before, after, source)
        if vector.get('strict_prefixes'):
            for length in range(len(expected)):
                rec = self.record(message, source, True)
                status = self.lib.wire_info_decode(C.byref(self.db), message,
                    C.byref(rec), output, length, C.byref(used))
                if status == 0 or used.value != 0:
                    raise ValueError(f'accepted truncated prefix of {length} bytes')
        # The same encoder must reject a buffer one byte short.
        if expected:
            status = self.lib.wire_info_encode(C.byref(self.db), message,
                C.byref(encoded_record), output, len(expected) - 1, C.byref(used))
            if status == 0 or used.value != 0:
                raise ValueError('accepted undersized output')
        self.keep.clear()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--library', required=True)
    p.add_argument('--images', type=Path, required=True)
    p.add_argument('sources', type=Path, nargs='+')
    args = p.parse_args()
    total = 0
    for path in args.sources:
        source = load_source(path)
        runtime = Runtime(args.library, (args.images / (path.stem + '.wi')).read_bytes())
        count = 0
        for message in source['protocol']['messages']:
            index = runtime.lib.wire_info_find(C.byref(runtime.db), message['name'].encode())
            if index < 0: raise ValueError(f'missing message {message["name"]}')
            for vector in message.get('vectors', []):
                try:
                    runtime.vector(index, vector)
                except ValueError as exc:
                    raise ValueError(f'{path.name}/{message["name"]}/{vector.get("name", "vector")}: {exc}') from exc
                count += 1
        print(f'{path.name}: {count} golden vectors passed')
        total += count
    if total == 0: raise ValueError('no vectors were tested')
    print(f'{total} vectors passed through wire_info only')

if __name__ == '__main__':
    main()
