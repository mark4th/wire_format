/* Named-record .wf compilation. Serialization is emitted as wire_format RPN
 * strings; field metadata contains only names, types and child table indices. */
#include "wfc.h"
#include "json5.h"
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIMIT (16u * 1024u * 1024u)
#define WI_FLAG_FILE_CRC32C (UINT32_C(1) << 31)
typedef const json5_value_t J;
typedef struct { char *data; size_t size, capacity; } buffer;
typedef struct {
    wfc_error_t *error;
    J *messages;
    size_t current;
    int failed;
} compiler;
static const char *types[] = { "", "uint", "bytes", "sdnv", "records", "parameter",
    "computed", "assert", "mark", "crc", "cbor-uint", "cbor-bytes", "cbor-text",
    "cbor-array", "decimal", "bcd", "terminated-uint" };
static unsigned char c_token_char(unsigned char ch)
{
    if (ch >= 'a' && ch <= 'z') { return (unsigned char)(ch - 'a' + 'A'); }
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) { return ch; }
    return '_';
}
static void c_token(char output[192], const char *text)
{
    size_t i;
    for(i=0;text[i] && i<191;i++) { output[i]=(char)c_token_char((unsigned char)text[i]); }
    output[i]=0;
}
static int c_token_equal(const char *a, const char *b)
{
    while(*a && *b)
    {
        if(c_token_char((unsigned char)*a++)!=c_token_char((unsigned char)*b++)) { return 0; }
    }
    return *a==*b;
}
static J *get(J *v, const char *name)
{
    size_t i;
    if (v == NULL || v->type != JSON5_OBJECT) { return NULL; }
    for (i = 0; i < v->as.object.count; i++)
    { if (strcmp(v->as.object.members[i].name, name) == 0) { return v->as.object.members[i].value; } }
    return NULL;
}
static int fail(compiler *c, J *v, const char *message)
{
    if (!c->failed) { wfc_set_error(c->error, (wfc_location_t){v ? v->line : 1, v ? v->column : 1}, "%s", message); }
    c->failed = 1;
    return 0;
}
static int append(compiler *c, buffer *b, const void *data, size_t n)
{
    if (c->failed) { return 0; }
    if (n > LIMIT || b->size > LIMIT - n) { return fail(c, NULL, "compiled output exceeds 16 MiB"); }
    if (b->size + n + 1 > b->capacity)
    {
        size_t cap = (b->size + n + 1) * 2;
        char *p = realloc(b->data, cap);
        if (p == NULL) { return fail(c, NULL, "out of memory"); }
        b->data = p; b->capacity = cap;
    }
    if (n) { memcpy(b->data + b->size, data, n); }
    b->size += n; b->data[b->size] = 0;
    return 1;
}
static void emit(compiler *c, buffer *b, const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    int n;
    va_start(ap, fmt); n = vsnprintf(text, sizeof(text), fmt, ap); va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(text)) { fail(c, NULL, "format operand too long"); return; }
    append(c, b, text, (size_t)n);
}
static const char *string(compiler *c, J *v, int identifier)
{
    size_t i;
    if (v == NULL || v->type != JSON5_STRING || v->as.scalar.length == 0 ||
        strlen(v->as.scalar.text) != v->as.scalar.length)
    { fail(c, v, "expected a nonempty string without NUL"); return ""; }
    if (identifier)
    {
        if (v->as.scalar.length >= 192) { fail(c, v, "identifier too long"); return ""; }
        for (i = 0; i < v->as.scalar.length; i++)
        {
            unsigned char ch = (unsigned char)v->as.scalar.text[i];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.'))
            { fail(c, v, "identifier must use letters, digits, hyphen, underscore or dot"); return ""; }
        }
    }
    return v->as.scalar.text;
}
static int shape(compiler *c, J *v, const char *allowed)
{
    size_t i, j;
    if (v == NULL || v->type != JSON5_OBJECT) { return fail(c, v, "expected an object"); }
    for (i = 0; i < v->as.object.count; i++)
    {
        const json5_member_t *m = &v->as.object.members[i];
        char key[128];
        if (m->name_length != strlen(m->name) || snprintf(key, sizeof(key), "|%s|", m->name) >= (int)sizeof(key) || !strstr(allowed, key))
        { return fail(c, m->value, "unknown property"); }
        for (j = 0; j < i; j++)
        { if (!strcmp(m->name, v->as.object.members[j].name)) { return fail(c, m->value, "duplicate property"); } }
    }
    return 1;
}
static uint64_t number(compiler *c, J *v)
{
    char *end;
    unsigned long long n;
    const char *s;
    if (v == NULL || v->type != JSON5_NUMBER) { fail(c, v, "expected unsigned integer"); return 0; }
    s = v->as.scalar.text;
    if (*s == '-') { fail(c, v, "expected unsigned integer"); return 0; }
    errno = 0; n = strtoull(s, &end, (strstr(s, "0x") || strstr(s, "0X")) ? 16 : 10);
    if (errno || end == s || *end || n > UINT64_MAX) { fail(c, v, "expected unsigned 64-bit integer"); return 0; }
    return (uint64_t)n;
}
static int kind(compiler *c, J *field)
{
    const char *s = string(c, get(field, "type"), 1);
    int i;
    for (i = 1; i < 17; i++) { if (!strcmp(types[i], s)) { return i; } }
    fail(c, field, "unknown field type"); return 0;
}
static int message_id(compiler *c, const char *name)
{
    size_t i;
    for (i = 0; i < c->messages->as.array.count; i++)
    { if (!strcmp(string(c, get(c->messages->as.array.items[i], "name"), 1), name)) { return (int)i; } }
    return -1;
}
static int local_field(compiler *c, size_t message, const char *name)
{
    J *fields = get(c->messages->as.array.items[message], "fields");
    size_t i;
    if (fields == NULL || fields->type != JSON5_ARRAY) { return 0; }
    for (i = 0; i < fields->as.array.count; i++)
    { if (!strcmp(string(c, get(fields->as.array.items[i], "name"), 1), name)) { return 1; } }
    return 0;
}
static uint64_t parents(compiler *c, uint64_t mask)
{
    uint64_t result = 0;
    size_t i, j;
    for (i = 0; i < c->messages->as.array.count; i++)
    {
        J *fields = get(c->messages->as.array.items[i], "fields");
        if (!fields || fields->type != JSON5_ARRAY) { continue; }
        for (j = 0; j < fields->as.array.count; j++)
        {
            J *child = get(fields->as.array.items[j], "message");
            if (child)
            {
                int id = message_id(c, string(c, child, 1));
                if (id >= 0 && (mask & (UINT64_C(1) << id))) { result |= UINT64_C(1) << i; }
            }
        }
    }
    return result;
}
static const char *reference(compiler *c, J *v)
{
    const char *name = string(c, v, 1), *local = name;
    uint64_t scopes = UINT64_C(1) << c->current, seen = 0;
    size_t i;
    while (!strncmp(local, "parent.", 7)) { scopes = parents(c, scopes); local += 7; }
    while (scopes)
    {
        for (i = 0; i < c->messages->as.array.count; i++)
        { if ((scopes & (UINT64_C(1) << i)) && local_field(c, i, local)) { return name; } }
        seen |= scopes; scopes = parents(c, scopes) & ~seen;
    }
    fail(c, v, "unknown field reference"); return name;
}
static void literal(compiler *c, buffer *b, uint64_t n) { emit(c, b, "%%{%" PRIu64 "}", n); }
static void expression(compiler *c, buffer *b, J *v, unsigned depth)
{
    J **a;
    const char *op;
    size_t n, i;
    static const char *binary[] = {"+","-","*","/","%","&","|","^","<<",">>","==","!=","<","<=",">",">=","and","or"};
    static const char codes[] = "+-*/m&|^[]=~<l>gAO";
    if (c->failed) { return; }
    if (depth > 32) { fail(c, v, "expression nesting exceeds 32"); return; }
    if (v && v->type == JSON5_NUMBER) { literal(c, b, number(c, v)); return; }
    if (v && v->type == JSON5_STRING) { emit(c, b, "%%g{%s}", reference(c, v)); return; }
    if (!v || v->type != JSON5_ARRAY || v->as.array.count == 0) { fail(c, v, "invalid expression"); return; }
    a = (J **)v->as.array.items; n = v->as.array.count - 1; op = string(c, a[0], 0);
    if ((!strcmp(op,"len") || !strcmp(op,"count")) && n == 1)
    { emit(c,b,"%%%c{%s}", !strcmp(op,"len") ? 'z' : 'k', reference(c,a[1])); return; }
    if (!strcmp(op,"byte") && n == 2)
    { expression(c,b,a[2],depth+1); emit(c,b,"%%uh{%s}",reference(c,a[1])); return; }
    if ((!strcmp(op,"item") && n == 3) || (!strcmp(op,"unique") && n == 2) || (!strcmp(op,"count-equal") && n == 3))
    {
        if (n == 3) { expression(c,b,a[3],depth+1); }
        emit(c,b,"%%u%c{%s}{%s}", !strcmp(op,"item") ? 'I' : !strcmp(op,"unique") ? 'U' : 'C',reference(c,a[1]),string(c,a[2],1)); return;
    }
    if (n == 0)
    {
        const char *queries[] = {"remaining","position","start","peek","index"};
        const char qcodes[] = "rpokn";
        for (i=0;i<5;i++) { if (!strcmp(op,queries[i])) { emit(c,b,"%%u%c",qcodes[i]); return; } }
    }
    if (n == 2)
    {
        for (i=0;i<18;i++) { if (!strcmp(op,binary[i])) {
            expression(c,b,a[1],depth+1); expression(c,b,a[2],depth+1); emit(c,b,"%%u%c",codes[i]); return; } }
    }
    if ((!strcmp(op,"select") && n==3) || (!strcmp(op,"not") && n==1))
    {
        for (i=1;i<=n;i++) { expression(c,b,a[i],depth+1); }
        emit(c,b,"%%u%c",n==3?'?':'!'); return;
    }
    fail(c,v,"unknown expression operator or wrong argument count");
}
static void expr_or(compiler *c, buffer *b, J *f, const char *key, uint64_t fallback)
{ J *v=get(f,key); if(v) { expression(c,b,v,0); } else { literal(c,b,fallback); } }
static int flag(compiler *c, J *f, const char *key)
{
    J *v = get(f,key);
    if (!v) { return 0; }
    if (v->type != JSON5_BOOLEAN) { fail(c,v,"flag must be boolean"); return 0; }
    return v->as.boolean;
}
static void field_program(compiler *c, buffer *b, J *f, int decode)
{
    const char *name = string(c,get(f,"name"),1);
    int t=kind(c,f), little=0;
    J *value=get(f,"constant"), *width=get(f,"width"), *when=get(f,"when");
    if (!value) { value=get(f,decode ? "decode_value":"value"); }
    if (when) { emit(c,b,"%%?"); expression(c,b,when,0); emit(c,b,"%%t"); }
    if (t!=1 && t!=5 && t!=6 && t!=7) { emit(c,b,"%%uf"); }
    if (get(f,"byte_order")) { little=!strcmp(string(c,get(f,"byte_order"),0),"little-endian"); }
    if (t==2 || t==11 || t==12)
    {
        if (t==2)
        {
            J *length=get(f,"length"); if (decode && !length) { length=get(f,"decode_length"); }
            if (length) { expression(c,b,length,0); emit(c,b,"%%%c{%s}",decode?'N':'V',name); }
            else { emit(c,b,"%%%c{%s}",decode?'R':'v',name); }
        }
        else
        {
            if (decode) { literal(c,b,0); } else { emit(c,b,"%%z{%s}",name); }
            literal(c,b,t==11?2:3); emit(c,b,"%%ub%%%c{%s}",decode?'N':'V',name);
        }
    }
    else if (t==4)
    {
        int child=message_id(c,string(c,get(f,"message"),1));
        expr_or(c,b,f,"count",get(f,"until")?UINT64_MAX:1);
        expr_or(c,b,f,"until",256); emit(c,b,"%%J[%d]{%s}",child,name);
    }
    else if (t==9)
    {
        expr_or(c,b,f,"algorithm",1);
        if (get(f,"from")) { expression(c,b,get(f,"from"),0); } else { emit(c,b,"%%uo"); }
        literal(c,b,(unsigned)flag(c,f,"cbor") | ((unsigned)flag(c,f,"to_end")<<1));
        emit(c,b,"%%uK{%s}",name);
    }
    else
    {
        if (t==8) { emit(c,b,"%%up"); }
        else if (t==7 || t==6) { expression(c,b,value ? value:get(f,"value"),0); }
        else if (decode && t!=5) { literal(c,b,0); }
        else if (value) { expression(c,b,value,0); }
        else { emit(c,b,"%%g{%s}",name); }
        /* Save the logical encode value before emitting; decode replaces it. */
        emit(c,b,"%%P{%s}",name);
        if (t==1)
        {
            if (!decode) { emit(c,b,"%%g{%s}",name); if(little) { expression(c,b,width,0); emit(c,b,"%%us"); } }
            expression(c,b,width,0); emit(c,b,"%%%c",decode?'j':'i');
            if (decode) { if(little) { expression(c,b,width,0); emit(c,b,"%%us"); } emit(c,b,"%%P{%s}",name); }
        }
        else if (t==3)
        {
            if (decode) { emit(c,b,"%%D%%P{%s}",name); } else { emit(c,b,"%%g{%s}%%d",name); }
        }
        else if (t==10 || t==13)
        { emit(c,b,"%%g{%s}",name); literal(c,b,t==10?0:4); emit(c,b,"%%ub%%P{%s}",name); }
        else if (t==14 || t==15)
        { emit(c,b,"%%g{%s}",name); expression(c,b,width,0); literal(c,b,t==15); emit(c,b,"%%ud%%P{%s}",name); }
        else if (t==16)
        {
            emit(c,b,"%%g{%s}",name); expr_or(c,b,f,"length",0); expr_or(c,b,f,"termination_mask",1);
            expr_or(c,b,f,"termination_value",1); emit(c,b,"%%uT%%P{%s}",name);
        }
        else if (t==7) { emit(c,b,"%%g{%s}",name); literal(c,b,8); emit(c,b,"%%ua"); }
    }
    if (decode && get(f,"constant"))
    { emit(c,b,"%%g{%s}",name); expression(c,b,get(f,"constant"),0); emit(c,b,"%%u="); literal(c,b,6); emit(c,b,"%%ua"); }
    if (get(f,"min"))
    { emit(c,b,"%%g{%s}",name); expression(c,b,get(f,"min"),0); emit(c,b,"%%ug"); literal(c,b,3); emit(c,b,"%%ua"); }
    if (get(f,"max"))
    { emit(c,b,"%%g{%s}",name); expression(c,b,get(f,"max"),0); emit(c,b,"%%ul"); literal(c,b,3); emit(c,b,"%%ua"); }
    if (when)
    { if (decode) { emit(c,b,"%%e%%uZ{%s}",name); } emit(c,b,"%%;"); }
}
static int validate(compiler *c, J *p)
{
    size_t i,j,k;
    unsigned depths[64]={0};
    const char *protocol_name;
    if (!shape(c,p,"|name||description||standard||reference||messages|")) { return 0; }
    protocol_name=string(c,get(p,"name"),1);
    if(!((protocol_name[0]>='a' && protocol_name[0]<='z') ||
         (protocol_name[0]>='A' && protocol_name[0]<='Z')))
    { return fail(c,get(p,"name"),"protocol name must begin with a letter for the generated C header"); }
    c->messages=get(p,"messages");
    if (!c->messages || c->messages->type!=JSON5_ARRAY || c->messages->as.array.count==0 || c->messages->as.array.count>64)
    { return fail(c,p,"requires 1..64 messages"); }
    for(i=0;i<c->messages->as.array.count;i++)
    {
        J *m=c->messages->as.array.items[i], *fields;
        const char *name;
        if (!shape(c,m,"|name||description||fields||vectors|")) { return 0; }
        name=string(c,get(m,"name"),1);
        for(j=0;j<i;j++)
        {
            const char *prior=string(c,get(c->messages->as.array.items[j],"name"),1);
            if(!strcmp(name,prior)) { return fail(c,m,"duplicate message name"); }
            if(c_token_equal(name,prior)) { return fail(c,m,"message names collide in the generated C header"); }
        }
        fields=get(m,"fields"); depths[i]=1;
        if (!fields || fields->type!=JSON5_ARRAY || fields->as.array.count>128) { return fail(c,m,"requires an array of at most 128 fields"); }
        for(j=0;j<fields->as.array.count;j++)
        {
            J *f=fields->as.array.items[j];
            int t;
            if (!shape(c,f,"|name||description||type||width||length||decode_length||value||constant||when||min||max||message||count||algorithm||from||until||cbor||to_end||decode_value||byte_order||termination_mask||termination_value|")) { return 0; }
            name=string(c,get(f,"name"),1); t=kind(c,f);
            for(k=0;k<j;k++)
            {
                const char *prior=string(c,get(fields->as.array.items[k],"name"),1);
                if(!strcmp(name,prior)) { return fail(c,f,"duplicate field name"); }
                if(c_token_equal(name,prior)) { return fail(c,f,"field names collide in the generated C header"); }
            }
            if ((get(f,"constant") && get(f,"value")) || (get(f,"length") && get(f,"decode_length"))) { return fail(c,f,"mutually exclusive field properties"); }
            if ((t==1 || t==14 || t==15) && !get(f,"width")) { return fail(c,f,"field requires width"); }
            if ((t==6 || t==7) && !get(f,"value")) { return fail(c,f,"field requires value"); }
            if ((get(f,"width") && t!=1 && t!=14 && t!=15) || (get(f,"length") && t!=2 && t!=16) ||
                (get(f,"decode_length") && t!=2) || (get(f,"decode_value") && t!=6) ||
                ((get(f,"algorithm") || get(f,"from") || get(f,"cbor") || get(f,"to_end")) && t!=9) ||
                ((get(f,"until") || get(f,"count") || get(f,"message")) && t!=4) ||
                (get(f,"byte_order") && t!=1) || ((get(f,"termination_mask") || get(f,"termination_value")) && t!=16))
            { return fail(c,f,"property does not apply to this field type"); }
            if (get(f,"byte_order"))
            { const char *order=string(c,get(f,"byte_order"),0); if(strcmp(order,"big-endian") && strcmp(order,"little-endian")) { return fail(c,f,"unknown byte_order"); } }
            if (get(f,"until") && number(c,get(f,"until"))>256) { return fail(c,f,"until must be a byte or 256 (EOF)"); }
            if(t==16)
            {
                uint64_t mask=get(f,"termination_mask")?number(c,get(f,"termination_mask")):1;
                uint64_t stop=get(f,"termination_value")?number(c,get(f,"termination_value")):1;
                if(mask==0 || mask>255 || (stop & ~mask)) { return fail(c,f,"invalid termination mask/value"); }
            }
            if(t==4)
            {
                int child=message_id(c,string(c,get(f,"message"),1));
                if(child<0 || (size_t)child>=i) { return fail(c,f,"record child must be an earlier message"); }
                if(depths[i]<depths[child]+1) { depths[i]=depths[child]+1; }
                if(depths[i]>7) { return fail(c,f,"record nesting exceeds 7"); }
            }
        }
    }
    return !c->failed;
}
static void put32(char *p, uint32_t n)
{ unsigned i; for(i=0;i<4;i++) { p[i]=(char)(n>>(8*i)); } }
static void word(compiler *c, buffer *b, uint32_t n)
{ char bytes[4]; put32(bytes,n); append(c,b,bytes,4); }
static void add_string(compiler *c, buffer *offsets, buffer *strings, const char *s)
{
    if(!s) { word(c,offsets,UINT32_MAX); return; }
    word(c,offsets,(uint32_t)strings->size); append(c,strings,s,strlen(s)+1);
}
int wfc_records_generate(const wfc_protocol_t *protocol, wfc_generated_t *out, wfc_error_t *error)
{
    compiler c={.error=error};
    J *p=get(protocol->record_source,"protocol");
    buffer strings={0}, offsets={0}, meta={0}, image={0};
    uint32_t sections[64], metadata[64];
    size_t i,j,count, string_offset, offset_base, meta_base;
    char header[64]={ 'W','I',0,0,4,0,64,0 }, row[32]={0};
    static const char *props[]={"name","description","standard","reference"};
    memset(out,0,sizeof(*out));
    if(!validate(&c,p)) { return 0; }
    count=c.messages->as.array.count;
    for(i=0;i<4;i++) { J *v=get(p,props[i]); add_string(&c,&offsets,&strings,v?string(&c,v,0):NULL); }
    for(i=0;i<count && !c.failed;i++)
    {
        J *m=c.messages->as.array.items[i], *fields=get(m,"fields");
        int decode;
        c.current=i; sections[i]=(uint32_t)offsets.size; metadata[i]=(uint32_t)meta.size;
        add_string(&c,&offsets,&strings,string(&c,get(m,"name"),1));
        add_string(&c,&offsets,&strings,get(m,"description")?string(&c,get(m,"description"),0):NULL);
        for(decode=0;decode<2;decode++)
        {
            buffer program={0};
            for(j=0;j<fields->as.array.count;j++) { field_program(&c,&program,fields->as.array.items[j],decode); }
            emit(&c,&program,"%%uF"); add_string(&c,&offsets,&strings,program.data?program.data:""); free(program.data);
        }
        for(j=0;j<fields->as.array.count;j++)
        {
            J *f=fields->as.array.items[j]; int t=kind(&c,f);
            add_string(&c,&offsets,&strings,string(&c,get(f,"name"),1));
            word(&c,&meta,(uint32_t)t);
            word(&c,&meta,t==4?(uint32_t)message_id(&c,string(&c,get(f,"message"),1)):UINT32_MAX);
        }
    }
    if(c.failed) { goto done; }
    meta_base=64+count*32; offset_base=meta_base+meta.size; string_offset=offset_base+offsets.size;
    put32(header+8,(uint32_t)(string_offset+strings.size));
    put32(header+16,(uint32_t)count); put32(header+20,32); put32(header+24,64);
    put32(header+28,(uint32_t)meta_base); put32(header+32,(uint32_t)meta.size);
    put32(header+36,(uint32_t)offset_base); put32(header+40,4);
    put32(header+44,(uint32_t)offset_base); put32(header+48,(uint32_t)(offsets.size/4));
    put32(header+52,(uint32_t)string_offset); put32(header+56,(uint32_t)strings.size);
    put32(header+12,WI_FLAG_FILE_CRC32C);
    append(&c,&image,header,64);
    for(i=0;i<count;i++)
    {
        size_t n=get(c.messages->as.array.items[i],"fields")->as.array.count;
        memset(row,0,32); put32(row,(uint32_t)offset_base+sections[i]); put32(row+4,(uint32_t)n+4);
        put32(row+12,(uint32_t)n); put32(row+24,2); put32(row+28,(uint32_t)meta_base+metadata[i]);
        append(&c,&image,row,32);
    }
    append(&c,&image,meta.data,meta.size); append(&c,&image,offsets.data,offsets.size); append(&c,&image,strings.data,strings.size);
    if(!c.failed) { wfc_wi_set_crc32c((uint8_t *)image.data,image.size); }
    if(!c.failed)
    {
        buffer h={0};
        char prefix[192];
        c_token(prefix,string(&c,get(p,"name"),1));
        emit(&c,&h,"/* Generated by wfc. Do not edit. */\n#ifndef WF_GENERATED_%s_H\n#define WF_GENERATED_%s_H\n\n",prefix,prefix);
        emit(&c,&h,"#define %s_WI_FORMAT_VERSION 4u\n",prefix);
        emit(&c,&h,"#define %s_WI_FILE_SIZE %zuu\n",prefix,image.size);
        emit(&c,&h,"#define %s_WI_FLAGS %uu\n",prefix,WI_FLAG_FILE_CRC32C);
        emit(&c,&h,"#define %s_WI_CRC32C %uu\n",prefix,wfc_wi_crc32c((const uint8_t *)image.data,image.size));
        emit(&c,&h,"#define %s_WI_MESSAGE_COUNT %zuu\n\n",prefix,count);
        for(i=0;i<count;i++)
        {
            J *m=c.messages->as.array.items[i], *fields=get(m,"fields");
            char message[192];
            c_token(message,string(&c,get(m,"name"),1));
            emit(&c,&h,"#define %s_MESSAGE_%s %zuu\n",prefix,message,i);
            emit(&c,&h,"#define %s_%s_FIELD_COUNT %zuu\n",prefix,message,fields->as.array.count);
            for(j=0;j<fields->as.array.count;j++)
            {
                J *f=fields->as.array.items[j];
                char field[192];
                int t=kind(&c,f);
                c_token(field,string(&c,get(f,"name"),1));
                emit(&c,&h,"#define %s_%s_ORDINAL_%s %zuu\n",prefix,message,field,j);
                emit(&c,&h,"#define %s_%s_TYPE_%s %du\n",prefix,message,field,t);
                if(t==4)
                {
                    char child[192];
                    c_token(child,string(&c,get(f,"message"),1));
                    emit(&c,&h,"#define %s_%s_CHILD_%s %s_MESSAGE_%s\n",prefix,message,field,prefix,child);
                }
            }
            emit(&c,&h,"\n");
        }
        emit(&c,&h,"#endif /* WF_GENERATED_%s_H */\n",prefix);
        out->header=h.data; out->binary=(uint8_t*)image.data; out->binary_size=image.size; image.data=NULL;
    }
done:
    free(strings.data); free(offsets.data); free(meta.data); free(image.data);
    return !c.failed;
}
int wfc_records_check(const wfc_protocol_t *p, wfc_summary_t *summary, wfc_error_t *error)
{
    wfc_generated_t out;
    memset(summary,0,sizeof(*summary));
    if(!wfc_records_generate(p,&out,error)) { return 0; }
    summary->messages=get(get(p->record_source,"protocol"),"messages")->as.array.count;
    wfc_generated_free(&out);
    return 1;
}
