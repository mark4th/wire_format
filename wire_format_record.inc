/* Additional generic format-string primitives for named records. This file is
 * part of wire_format.c; there is no second instruction loop or binary VM. */
static void record_fail(wi_vars_t *v, wire_info_status_t status)
{
    if (v->record_scope != NULL && v->record_scope->status == WIRE_INFO_OK)
    {
        v->record_scope->status = status;
    }
    v->overrun = 1;
}
static int record_name(wi_vars_t *v, char *name, size_t capacity)
{
    size_t n = 0;
    if (*v->f_str != '{')
    {
        record_fail(v, WIRE_INFO_SCHEMA);
        return 0;
    }
    v->f_str++;
    while (*v->f_str != 0 && *v->f_str != '}' && n + 1 < capacity)
    {
        name[n++] = (char)*v->f_str++;
    }
    if (*v->f_str != '}' || n == 0)
    {
        record_fail(v, WIRE_INFO_SCHEMA);
        return 0;
    }
    v->f_str++;
    name[n] = 0;
    return 1;
}
static wire_info_value_t *record_lookup(wi_vars_t *v, const char *name,
                                       wi_record_scope_t **owner, int *column)
{
    wi_record_scope_t *s = v->record_scope;
    while (strncmp(name, "parent.", 7) == 0)
    {
        s = s != NULL ? s->parent : NULL;
        name += 7;
    }
    for (; s != NULL; s = s->parent)
    {
        int i = s->field(s->metadata, s->message, name);
        if (i >= 0)
        {
            if (s->record == NULL || s->record->values == NULL || (size_t)i >= s->record->capacity)
            {
                record_fail(v, WIRE_INFO_CAPACITY);
                return NULL;
            }
            if (owner != NULL) { *owner = s; }
            if (column != NULL) { *column = i; }
            return &s->record->values[i];
        }
    }
    record_fail(v, WIRE_INFO_SCHEMA);
    return NULL;
}
static wire_info_value_t *record_operand(wi_vars_t *v)
{
    char name[192];
    return record_name(v, name, sizeof(name)) ? record_lookup(v, name, NULL, NULL) : NULL;
}
static void record_get(wi_vars_t *v)
{
    wire_info_value_t *field = record_operand(v);
    if (field != NULL) { fs_push(v, (int64_t)field->number); }
}
static void record_put(wi_vars_t *v)
{
    wire_info_value_t *field = record_operand(v);
    uint64_t value = (uint64_t)fs_pop(v);
    if (field != NULL && !v->overrun) { field->number = value; }
}
static size_t record_position(wi_vars_t *v)
{
    size_t offset = v->out != NULL ? v->out_len : v->in_pos - (v->read_bits != 0);
    return (v->record_scope != NULL ? v->record_scope->base : 0) + offset;
}
static int record_aligned(wi_vars_t *v)
{
    if (v->read_bits != 0 || v->write_bits != 0)
    {
        record_fail(v, WIRE_INFO_SCHEMA);
        return 0;
    }
    return 1;
}
static uint64_t record_bits(wi_vars_t *v, uint64_t value, uint64_t width)
{
    uint64_t result = 0;
    unsigned i;
    if (v->overrun) { return 0; }
    if (width > 64 || (v->out != NULL && width < 64 && (value >> width) != 0))
    {
        record_fail(v, WIRE_INFO_RANGE);
        return 0;
    }
    for (i = 0; i < width && !v->overrun; i++)
    {
        if (v->out != NULL)
        {
            v->bit_acc |= (uint8_t)(((value >> (width - i - 1)) & 1) << (7 - v->write_bits));
            if (++v->write_bits == 8)
            {
                b_emit(v, v->bit_acc);
                v->bit_acc = 0;
                v->write_bits = 0;
            }
        }
        else
        {
            if (v->read_bits == 0)
            {
                v->in_acc = b_read(v);
                v->read_bits = 8;
            }
            result = result << 1 | ((v->in_acc >> --v->read_bits) & 1);
        }
    }
    return v->out != NULL ? value : result;
}
static void record_emit_bits(wi_vars_t *v)
{
    uint64_t width = (uint64_t)fs_pop(v), value = (uint64_t)fs_pop(v);
    if (v->out == NULL) { record_fail(v, WIRE_INFO_SCHEMA); return; }
    record_bits(v, value, width);
}
static void record_read_bits(wi_vars_t *v)
{
    uint64_t width = (uint64_t)fs_pop(v);
    if (v->out != NULL) { record_fail(v, WIRE_INFO_SCHEMA); return; }
    fs_push(v, (int64_t)record_bits(v, 0, width));
}
static void record_slice(wi_vars_t *v, int exact)
{
    wire_info_value_t *f = record_operand(v);
    uint64_t length = exact ? (uint64_t)fs_pop(v) :
        v->out != NULL && f != NULL ? f->length : v->in_size - v->in_pos;
    if (f == NULL || v->overrun || !record_aligned(v)) { return; }
    if (v->out != NULL)
    {
        if (length != f->length || (length != 0 && f->bytes == NULL))
        { record_fail(v, WIRE_INFO_RANGE); return; }
        if (length > v->out_size - v->out_len)
        { record_fail(v, WIRE_INFO_CAPACITY); return; }
        if (length != 0) { memcpy(v->out + v->out_len, f->bytes, (size_t)length); }
        v->out_len += (size_t)length;
    }
    else
    {
        if (length > v->in_size - v->in_pos)
        { record_fail(v, WIRE_INFO_TRUNCATED); return; }
        f->bytes = v->in != NULL ? v->in + v->in_pos : NULL;
        f->length = (size_t)length;
        v->in_pos += (size_t)length;
    }
}
static void record_sequence(wi_vars_t *v, int n)
{
    wire_info_value_t *list = record_operand(v);
    uint64_t until = (uint64_t)fs_pop(v), count = (uint64_t)fs_pop(v);
    wi_record_scope_t *parent = v->record_scope;
    size_t i = 0;
    if (v->overrun || list == NULL || parent == NULL || !record_aligned(v)) { return; }
    if (n < 0 || n >= v->cur_fmt || n >= v->nfmts || v->fmts[n] == NULL || parent->depth + 1 >= WI_CALL_DEPTH)
    { record_fail(v, WIRE_INFO_SCHEMA); return; }
    if (v->out != NULL && count == UINT64_MAX) { count = list->count; }
    if ((v->out != NULL && count != list->count) || (count != UINT64_MAX && count > list->capacity))
    { record_fail(v, WIRE_INFO_CAPACITY); return; }
    while (i < count && !v->overrun)
    {
        wi_vars_t child;
        wi_record_scope_t scope = *parent;
        size_t before = record_position(v);
        if (v->out == NULL && count == UINT64_MAX)
        {
            if (v->in_pos == v->in_size)
            {
                if (until != 256) { record_fail(v, WIRE_INFO_TRUNCATED); }
                break;
            }
            if (v->in[v->in_pos] == until) { break; }
        }
        if (i >= list->capacity || list->records == NULL)
        { record_fail(v, WIRE_INFO_CAPACITY); return; }
        if (list->records[i].capacity < parent->count(parent->metadata, (unsigned)n) ||
            (parent->count(parent->metadata, (unsigned)n) != 0 && list->records[i].values == NULL))
        { record_fail(v, WIRE_INFO_CAPACITY); return; }
        scope.parent = parent;
        scope.record = &list->records[i];
        scope.message = (unsigned)n;
        scope.depth = parent->depth + 1;
        scope.base = before;
        scope.index = i;
        scope.crc_value = NULL;
        scope.status = WIRE_INFO_OK;
        if (v->out != NULL)
        {
            wi_init(&child, v->out + v->out_len, v->out_size - v->out_len, NULL, 0);
        }
        else
        {
            wi_decode_init(&child, v->in + v->in_pos, v->in_size - v->in_pos, NULL, 0);
            list->count = i;
        }
        child.fmts = v->fmts;
        child.nfmts = v->nfmts;
        child.record_scope = &scope;
        wi_parse_rank(&child, child.fmts[n], n);
        if (child.overrun || child.read_bits != 0 || child.write_bits != 0 || child.fsp != 0)
        {
            record_fail(v, scope.status != WIRE_INFO_OK ? scope.status :
                child.overrun ? (v->out != NULL ? WIRE_INFO_CAPACITY : WIRE_INFO_TRUNCATED) : WIRE_INFO_SCHEMA);
            return;
        }
        if (v->out != NULL) { v->out_len += child.out_len; }
        else { v->in_pos += child.in_pos; }
        if (record_position(v) == before && count == UINT64_MAX)
        { record_fail(v, WIRE_INFO_SCHEMA); return; }
        i++;
    }
    if (v->out == NULL) { list->count = i; }
}
static uint64_t record_cbor(wi_vars_t *v, unsigned major, uint64_t value)
{
    uint64_t code, result;
    unsigned width;
    if (!record_aligned(v) || v->overrun) { return 0; }
    if (v->out != NULL)
    {
        if (major == 4 && value == UINT64_MAX) { b_emit(v, 0x9f); return value; }
        width = value < 24 ? 0 : value <= UINT8_MAX ? 8 : value <= UINT16_MAX ? 16 : value <= UINT32_MAX ? 32 : 64;
        code = width == 0 ? value : width == 8 ? 24 : width == 16 ? 25 : width == 32 ? 26 : 27;
        b_emit(v, (uint8_t)(major * 32 + code));
        if (width != 0) { record_bits(v, value, width); }
        return value;
    }
    code = b_read(v);
    if ((code >> 5) != major) { record_fail(v, WIRE_INFO_CONSTANT); return 0; }
    code &= 31;
    if (major == 4 && code == 31) { return UINT64_MAX; }
    if (code < 24) { return code; }
    if (code > 27) { record_fail(v, WIRE_INFO_RANGE); return 0; }
    width = 8u << (code - 24);
    result = record_bits(v, 0, width);
    if (result < 24 || (width > 8 && result <= (UINT64_MAX >> (64 - width / 2))))
    { record_fail(v, WIRE_INFO_RANGE); }
    return result;
}

static void record_length(wi_vars_t *v, int count)
{
    wire_info_value_t *f = record_operand(v);
    if (f != NULL) { fs_push(v, (int64_t)(count ? f->count : f->length)); }
}
static uint32_t record_checksum(const uint8_t *data, size_t begin, size_t end,
                               size_t zero, size_t zero_length, unsigned algorithm)
{
    uint32_t crc = algorithm == 3 ? UINT32_MAX : UINT16_MAX;
    size_t i;
    unsigned j;
    for (i = begin; i < end; i++)
    {
        uint8_t octet = i >= zero && i - zero < zero_length ? 0 : data[i];
        crc ^= algorithm == 1 ? (uint32_t)octet << 8 : octet;
        for (j = 0; j < 8; j++)
        {
            if (algorithm == 1)
            { crc = ((crc & 0x8000u) != 0 ? (crc << 1) ^ 0x1021u : crc << 1) & 0xffffu; }
            else
            { crc = (crc & 1u) != 0 ? (crc >> 1) ^ (algorithm == 2 ? 0x8408u : 0x82f63b78u) : crc >> 1; }
        }
    }
    return algorithm == 1 ? crc : algorithm == 2 ? (~crc & 0xffffu) : ~crc;
}
static void record_crc_finish(wi_vars_t *v, size_t end)
{
    wi_record_scope_t *s = v->record_scope;
    unsigned width, j;
    uint32_t actual;
    if (s == NULL || s->crc_value == NULL || v->overrun) { return; }
    width = s->crc_algorithm == 3 ? 4 : 2;
    actual = record_checksum(v->out != NULL ? s->output : s->input,
        s->crc_from, end, s->crc_at, width, s->crc_algorithm);
    if (v->out != NULL)
    {
        for (j = 0; j < width; j++)
        { s->output[s->crc_at + j] = (uint8_t)(actual >> ((width - j - 1) * 8)); }
        s->crc_value->number = actual;
    }
    else if (s->crc_value->number != actual) { record_fail(v, WIRE_INFO_CHECKSUM); }
    s->crc_value = NULL;
}
static void record_crc(wi_vars_t *v)
{
    wire_info_value_t *f = record_operand(v);
    uint64_t flags = (uint64_t)fs_pop(v), from = (uint64_t)fs_pop(v), algorithm = (uint64_t)fs_pop(v);
    wi_record_scope_t *s = v->record_scope;
    unsigned width = algorithm == 3 ? 32 : 16;
    size_t at;
    if (v->overrun || f == NULL || s == NULL || !record_aligned(v)) { return; }
    if (algorithm < 1 || algorithm > 3 || flags > 3 || s->crc_value != NULL)
    { record_fail(v, WIRE_INFO_SCHEMA); return; }
    if ((flags & 1) != 0 && record_cbor(v, 2, width / 8) != width / 8)
    { record_fail(v, WIRE_INFO_CHECKSUM); return; }
    at = record_position(v);
    f->number = record_bits(v, 0, width);
    if (v->overrun) { return; }
    if (from > at) { record_fail(v, WIRE_INFO_RANGE); return; }
    s->crc_value = f;
    s->crc_at = at;
    s->crc_from = (size_t)from;
    s->crc_algorithm = (unsigned)algorithm;
    if ((flags & 2) == 0) { record_crc_finish(v, (flags & 1) != 0 ? record_position(v) : at); }
}
static void record_decimal(wi_vars_t *v)
{
    uint64_t mode = (uint64_t)fs_pop(v), width = (uint64_t)fs_pop(v), value = (uint64_t)fs_pop(v);
    uint64_t divisor = 1, result = 0;
    unsigned j;
    if (width == 0 || width > 19 || mode > 1)
    { record_fail(v, WIRE_INFO_RANGE); return; }
    for (j = 1; j < width; j++) { divisor *= 10; }
    if (v->out != NULL && value / divisor > 9) { record_fail(v, WIRE_INFO_RANGE); return; }
    for (j = 0; j < width && !v->overrun; j++)
    {
        uint64_t digit = record_bits(v, value / divisor % 10 + (mode == 0 ? '0' : 0), mode == 0 ? 8 : 4);
        if (v->out == NULL)
        {
            if (mode == 0)
            {
                if (digit < '0') { record_fail(v, WIRE_INFO_RANGE); return; }
                digit -= '0';
            }
            if (digit > 9) { record_fail(v, WIRE_INFO_RANGE); return; }
            result = result * 10 + digit;
        }
        divisor /= 10;
    }
    fs_push(v, (int64_t)(v->out != NULL ? value : result));
}
static void record_terminated(wi_vars_t *v)
{
    uint64_t stop = (uint64_t)fs_pop(v), mask = (uint64_t)fs_pop(v);
    uint64_t n = (uint64_t)fs_pop(v), value = (uint64_t)fs_pop(v), temp = value;
    unsigned j;
    if (mask == 0 || mask > 255 || (stop & ~mask) != 0)
    { record_fail(v, WIRE_INFO_SCHEMA); return; }
    if (v->out == NULL)
    {
        value = 0;
        for (j = 0; j < 8 && !v->overrun; j++)
        {
            temp = record_bits(v, 0, 8);
            value = value << 8 | temp;
            if ((temp & mask) == stop) { break; }
        }
        if (j == 8) { record_fail(v, WIRE_INFO_RANGE); }
    }
    else
    {
        if (n == 0) { n = 1; while (temp > 255) { temp >>= 8; n++; } }
        if (n > 8 || (n < 8 && value >> (n * 8) != 0)) { record_fail(v, WIRE_INFO_RANGE); return; }
        for (j = 0; j < n && !v->overrun; j++)
        {
            temp = (value >> ((n - j - 1) * 8)) & 255;
            if (((temp & mask) == stop) != (j + 1 == n)) { record_fail(v, WIRE_INFO_RANGE); return; }
            record_bits(v, temp, 8);
        }
    }
    fs_push(v, (int64_t)value);
}
static void record_list_query(wi_vars_t *v, int op)
{
    char name[192], member[192];
    wi_record_scope_t *owner = NULL;
    wire_info_value_t *list;
    uint64_t arg = op == 'U' ? 0 : (uint64_t)fs_pop(v), result = op == 'U';
    int column = -1, child, field;
    size_t i, j;
    if (!record_name(v, name, sizeof(name)) || !record_name(v, member, sizeof(member))) { return; }
    list = record_lookup(v, name, &owner, &column);
    if (list == NULL || v->overrun) { return; }
    child = owner->child(owner->metadata, owner->message, (unsigned)column);
    field = child < 0 ? -1 : owner->field(owner->metadata, (unsigned)child, member);
    if (field < 0) { record_fail(v, WIRE_INFO_SCHEMA); return; }
    if (list->count > list->capacity || (list->count != 0 && list->records == NULL))
    { record_fail(v, WIRE_INFO_CAPACITY); return; }
    for (i = 0; i < list->count; i++)
    {
        if (list->records[i].values == NULL || (size_t)field >= list->records[i].capacity)
        { record_fail(v, WIRE_INFO_CAPACITY); return; }
    }
    if (op == 'I')
    {
        if (arg >= list->count) { record_fail(v, WIRE_INFO_RANGE); return; }
        result = list->records[arg].values[field].number;
    }
    else for (i = 0; i < list->count; i++)
    {
        uint64_t value = list->records[i].values[field].number;
        if (op == 'C') { result += value == arg; }
        else for (j = 0; j < i; j++)
        {
            if (list->records[j].values[field].number == value) { result = 0; break; }
        }
        if (op == 'U' && result == 0) { break; }
    }
    fs_push(v, (int64_t)result);
}
/* %u introduces unsigned/record extensions to the same RPN instruction set.
 * Operands remain ordinary format strings and use the existing value stack. */
static void record_extension(wi_vars_t *v)
{
    int op = *v->f_str;
    uint64_t a, b, result = 0;
    wi_record_scope_t *s = v->record_scope;
    wire_info_value_t *f;
    if (op == 0) { record_fail(v, WIRE_INFO_SCHEMA); return; }
    v->f_str++;
    switch (op)
    {
        case 'Z':
            f = record_operand(v);
            if (f != NULL) { f->number = 0; f->bytes = NULL; f->length = 0; f->count = 0; }
            return;
        case 'a':
            a = (uint64_t)fs_pop(v); b = (uint64_t)fs_pop(v);
            if (a > WIRE_INFO_CONSTRAINT || a == 0) { record_fail(v, WIRE_INFO_SCHEMA); }
            else if (b == 0) { record_fail(v, (wire_info_status_t)a); }
            return;
        case 'f': record_aligned(v); return;
        case 'F': if (record_aligned(v)) { record_crc_finish(v, record_position(v)); } return;
        case 'K': record_crc(v); return;
        case 'I': case 'U': case 'C': record_list_query(v, op); return;
        case 'd': record_decimal(v); return;
        case 'T': record_terminated(v); return;
        case 'b':
            a = (uint64_t)fs_pop(v); b = (uint64_t)fs_pop(v);
            if (a > 4) { record_fail(v, WIRE_INFO_SCHEMA); return; }
            fs_push(v, (int64_t)record_cbor(v, (unsigned)a, b)); return;
        case 's':
            a = (uint64_t)fs_pop(v); b = (uint64_t)fs_pop(v);
            if (a == 0 || a > 64 || a % 8 != 0 || (a < 64 && (b >> a) != 0))
            { record_fail(v, WIRE_INFO_RANGE); return; }
            while (a != 0) { result = result << 8 | (b & 255); b >>= 8; a -= 8; }
            fs_push(v, (int64_t)result); return;
        case 'h':
            a = (uint64_t)fs_pop(v); f = record_operand(v);
            if (f == NULL) { return; }
            if (a >= f->length || f->bytes == NULL) { record_fail(v, WIRE_INFO_RANGE); return; }
            fs_push(v, f->bytes[a]); return;
        case 'r': fs_push(v, (int64_t)(v->out != NULL ? v->out_size - v->out_len : v->in_size - v->in_pos)); return;
        case 'p': fs_push(v, (int64_t)record_position(v)); return;
        case 'o': fs_push(v, s != NULL ? (int64_t)s->base : 0); return;
        case 'n': fs_push(v, s != NULL ? (int64_t)s->index : 0); return;
        case 'k':
            if (v->in == NULL || v->in_pos >= v->in_size) { record_fail(v, WIRE_INFO_TRUNCATED); return; }
            fs_push(v, v->in[v->in_pos]); return;
        case '?':
            a = (uint64_t)fs_pop(v); b = (uint64_t)fs_pop(v);
            result = fs_pop(v) != 0 ? b : a;
            fs_push(v, (int64_t)result); return;
        case '!': result = fs_pop(v) == 0; fs_push(v, (int64_t)result); return;
        default: break;
    }
    b = (uint64_t)fs_pop(v); a = (uint64_t)fs_pop(v);
    if (v->overrun) { return; }
    switch (op)
    {
        case '+': if (a > UINT64_MAX - b) { record_fail(v, WIRE_INFO_RANGE); } result = a + b; break;
        case '-': if (a < b) { record_fail(v, WIRE_INFO_RANGE); } result = a - b; break;
        case '*': if (b != 0 && a > UINT64_MAX / b) { record_fail(v, WIRE_INFO_RANGE); } result = a * b; break;
        case '/': case 'm':
            if (b == 0) { record_fail(v, WIRE_INFO_RANGE); return; }
            result = op == '/' ? a / b : a % b; break;
        case '&': result = a & b; break;
        case '|': result = a | b; break;
        case '^': result = a ^ b; break;
        case '[':
            if (b > 63) { record_fail(v, WIRE_INFO_RANGE); return; }
            result = a << b; break;
        case ']':
            if (b > 63) { record_fail(v, WIRE_INFO_RANGE); return; }
            result = a >> b; break;
        case '=': result = a == b; break;
        case '~': result = a != b; break;
        case '<': result = a < b; break;
        case 'l': result = a <= b; break;
        case '>': result = a > b; break;
        case 'g': result = a >= b; break;
        case 'A': result = a != 0 && b != 0; break;
        case 'O': result = a != 0 || b != 0; break;
        default: record_fail(v, WIRE_INFO_SCHEMA); return;
    }
    fs_push(v, (int64_t)result);
}
