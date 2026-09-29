/* .wi table loader and named-record adapter for the wire_format interpreter.
 * This file contains no serialization engine or protocol dispatch. */
#include "wire_info.h"
#include "wire_format.h"
#include <string.h>

static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static const uint8_t *message_at(const wire_info_t *db, unsigned message)
{
    if (db == NULL || db->data == NULL || message >= db->messages) { return NULL; }
    return db->data + u32(db->data+24) + message*32;
}
static const char *slot(const wire_info_t *db, const uint8_t *m, unsigned index)
{
    uint32_t offset;
    if (m == NULL || index >= u32(m+4)) { return NULL; }
    offset=u32(db->data+u32(m)+index*4);
    return offset==UINT32_MAX ? NULL : (const char*)db->data+u32(db->data+52)+offset;
}
const char *wire_info_format(const wire_info_t *db, unsigned message, int decode)
{ return slot(db,message_at(db,message),decode?3:2); }
const char *wire_info_message_name(const wire_info_t *db, unsigned message)
{ return slot(db,message_at(db,message),0); }
size_t wire_info_field_count(const wire_info_t *db, unsigned message)
{ const uint8_t *m=message_at(db,message); return m?u32(m+12):0; }
const char *wire_info_field_name(const wire_info_t *db, unsigned message, unsigned field)
{
    const uint8_t *m=message_at(db,message);
    return m && field<u32(m+12)?slot(db,m,field+4):NULL;
}
wire_info_type_t wire_info_field_type(const wire_info_t *db, unsigned message, unsigned field)
{
    const uint8_t *m=message_at(db,message);
    return m && field<u32(m+12)?(wire_info_type_t)u32(db->data+u32(m+28)+field*8):0;
}
int wire_info_child(const wire_info_t *db, unsigned message, unsigned field)
{
    const uint8_t *m=message_at(db,message);
    if (!m || field>=u32(m+12) || wire_info_field_type(db,message,field)!=WIRE_INFO_RECORDS) { return -1; }
    return (int)u32(db->data+u32(m+28)+field*8+4);
}
int wire_info_find(const wire_info_t *db, const char *name)
{
    unsigned i;
    if (db==NULL || db->data==NULL || name==NULL) { return -1; }
    for(i=0;i<db->messages;i++) { if(!strcmp(wire_info_message_name(db,i),name)) { return (int)i; } }
    return -1;
}
int wire_info_field(const wire_info_t *db, unsigned message, const char *name)
{
    unsigned i;
    if(name==NULL) { return -1; }
    for(i=0;i<wire_info_field_count(db,message);i++) { if(!strcmp(wire_info_field_name(db,message,i),name)) { return (int)i; } }
    return -1;
}
static int identifier(const char **text)
{
    const char *p=*text;
    size_t n=0;
    if (*p++!='{') { return 0; }
    while (*p && *p!='}')
    {
        unsigned char ch=(unsigned char)*p++;
        if (!((ch>='a' && ch<='z') || (ch>='A' && ch<='Z') || (ch>='0' && ch<='9') || ch=='-' || ch=='_' || ch=='.')) { return 0; }
        if (++n>=192) { return 0; }
    }
    if (*p!='}' || n==0) { return 0; }
    *text=p+1; return 1;
}
/* Validate the safe named-record dialect before making borrowed strings
 * available. The actual execution uses wi_parse, including its RPN stack. */
static int program_valid(const char *p, unsigned rank)
{
    unsigned depth=0;
    unsigned char branch[64]={0};
    if(p==NULL) { return 0; }
    while(*p)
    {
        char op;
        if(*p++!='%' || (op=*p++)==0) { return 0; }
        switch(op)
        {
            case '{':
            {
                uint64_t n=0;
                if(*p<'0' || *p>'9') { return 0; }
                while(*p>='0' && *p<='9')
                { unsigned d=(unsigned)(*p++-'0'); if(n>(UINT64_MAX-d)/10) { return 0; } n=n*10+d; }
                if(*p++!='}') { return 0; }
                break;
            }
            case 'g': case 'P': case 'z': case 'k': case 'v': case 'V': case 'N': case 'R':
                if(!identifier(&p)) { return 0; } break;
            case 'J':
            {
                unsigned n=0,d=0;
                if(*p++!='[') { return 0; }
                while(*p>='0' && *p<='9') { if(++d>2) { return 0; } n=n*10+(unsigned)(*p++-'0'); }
                if(!d || n>=rank || *p++!=']' || !identifier(&p)) { return 0; }
                break;
            }
            case 'u':
                op=*p++;
                if(op==0) { return 0; }
                if(strchr("ZKh",op)) { if(!identifier(&p)) { return 0; } }
                else if(strchr("IUC",op)) { if(!identifier(&p) || !identifier(&p)) { return 0; } }
                else if(!strchr("afFdbsTrponk?!+-*/m&|^[]=~<l>gAO",op)) { return 0; }
                break;
            case '?': if(depth==64) { return 0; } branch[depth++]=0; break;
            case 't': if(!depth || branch[depth-1]!=0) { return 0; } branch[depth-1]=1; break;
            case 'e': if(!depth || branch[depth-1]!=1) { return 0; } branch[depth-1]=2; break;
            case ';': if(!depth || branch[depth-1]==0) { return 0; } depth--; break;
            case 'i': case 'j': case 'd': case 'D': break;
            default: return 0;
        }
    }
    return depth==0;
}
wire_info_status_t wire_info_open(wire_info_t *db, const void *data, size_t size)
{
    const uint8_t *p=data;
    wire_info_t temp;
    uint64_t messages_end,meta_end,offset_end,strings_end;
    uint32_t count,i,j,total=0, expected_meta,expected_section;
    if(db==NULL) { return WIRE_INFO_ARGUMENT; }
    memset(db,0,sizeof(*db));
    if(p==NULL || size<64) { return WIRE_INFO_SCHEMA; }
    count=u32(p+16);
    if(memcmp(p,"WI\0\0",4) || p[4]!=4 || p[5]!=0 || p[6]!=64 || p[7]!=0 ||
       u32(p+8)!=size || u32(p+12)!=0 || count==0 || count>WI_MAX_FORMATS ||
       u32(p+20)!=32 || u32(p+24)!=64 || u32(p+40)!=4 || u32(p+60)!=0) { return WIRE_INFO_SCHEMA; }
    messages_end=64+(uint64_t)count*32;
    meta_end=(uint64_t)u32(p+28)+u32(p+32);
    offset_end=(uint64_t)u32(p+44)+(uint64_t)u32(p+48)*4;
    strings_end=(uint64_t)u32(p+52)+u32(p+56);
    if(u32(p+28)!=messages_end || u32(p+36)!=meta_end || u32(p+44)!=meta_end ||
       u32(p+48)<4 || u32(p+52)!=offset_end || strings_end!=size ||
       messages_end>size || meta_end>size || offset_end>size) { return WIRE_INFO_SCHEMA; }
    for(i=0;i<u32(p+48);i++)
    {
        uint32_t offset=u32(p+u32(p+44)+i*4);
        if(offset!=UINT32_MAX && (offset>=u32(p+56) || memchr(p+u32(p+52)+offset,0,u32(p+56)-offset)==NULL)) { return WIRE_INFO_SCHEMA; }
    }
    temp=(wire_info_t){p,size,count,0};
    expected_meta=u32(p+28); expected_section=u32(p+44)+16;
    for(i=0;i<count;i++)
    {
        const uint8_t *m=message_at(&temp,i);
        uint32_t n=u32(m+12);
        if(n>128 || u32(m)!=expected_section || u32(m+4)!=n+4 ||
           (uint64_t)u32(m)+(uint64_t)(n+4)*4>offset_end || u32(m+8)!=0 || u32(m+16)!=0 || u32(m+20)!=0 ||
           u32(m+24)!=2 || u32(m+28)!=expected_meta || (uint64_t)expected_meta+(uint64_t)n*8>meta_end) { return WIRE_INFO_SCHEMA; }
        if(!slot(&temp,m,0) || !*slot(&temp,m,0) || !program_valid(slot(&temp,m,2),i) || !program_valid(slot(&temp,m,3),i)) { return WIRE_INFO_SCHEMA; }
        for(j=0;j<n;j++)
        {
            const uint8_t *f=p+expected_meta+j*8;
            uint32_t t=u32(f),child=u32(f+4);
            if(!slot(&temp,m,j+4) || !*slot(&temp,m,j+4) || t<1 || t>16 || (t==4 ? child>=i : child!=UINT32_MAX)) { return WIRE_INFO_SCHEMA; }
            {
                uint32_t k;
                for(k=0;k<j;k++) { if(!strcmp(slot(&temp,m,k+4),slot(&temp,m,j+4))) { return WIRE_INFO_SCHEMA; } }
            }
        }
        for(j=0;j<i;j++) { if(!strcmp(wire_info_message_name(&temp,j),slot(&temp,m,0))) { return WIRE_INFO_SCHEMA; } }
        expected_section+=(n+4)*4; expected_meta+=n*8; total+=n;
    }
    if(expected_meta!=meta_end || expected_section!=offset_end) { return WIRE_INFO_SCHEMA; }
    temp.fields=total;
    *db=temp;
    return WIRE_INFO_OK;
}
static int scope_field(const void *db, unsigned msg, const char *name)
{ return wire_info_field(db,msg,name); }
static int scope_child(const void *db, unsigned msg, unsigned field)
{ return wire_info_child(db,msg,field); }
static size_t scope_count(const void *db, unsigned msg)
{ return wire_info_field_count(db,msg); }
static wire_info_status_t execute(const wire_info_t *db, unsigned message, wire_info_record_t *record,
    const uint8_t *input, uint8_t *output, size_t size, size_t *used, int decode)
{
    wi_vars_t v;
    wi_record_scope_t scope={0};
    const char *formats[WI_MAX_FORMATS];
    uint8_t dummy=0;
    unsigned i;
    if(used) { *used=0; }
    if(!used || !db || !db->data || message>=db->messages || !record || (size && (decode ? input==NULL : output==NULL))) { return WIRE_INFO_ARGUMENT; }
    if(record->capacity<wire_info_field_count(db,message) || (wire_info_field_count(db,message) && !record->values)) { return WIRE_INFO_CAPACITY; }
    for(i=0;i<db->messages;i++) { formats[i]=wire_info_format(db,i,decode); }
    if(decode) { wi_decode_init(&v,input?input:&dummy,size,NULL,0); }
    else { wi_init(&v,output?output:&dummy,size,NULL,0); }
    scope.record=record; scope.metadata=db; scope.field=scope_field; scope.child=scope_child; scope.count=scope_count;
    scope.message=message; scope.input=input; scope.output=output;
    v.record_scope=&scope;
    if(wi_set_formats(&v,formats,(int)db->messages)) { return WIRE_INFO_SCHEMA; }
    wi_parse(&v,formats[message]);
    if(scope.status!=WIRE_INFO_OK) { return scope.status; }
    if(v.overrun) { return decode?WIRE_INFO_TRUNCATED:WIRE_INFO_CAPACITY; }
    if(v.read_bits || v.write_bits || v.fsp) { return WIRE_INFO_SCHEMA; }
    *used=decode?v.in_pos:v.out_len;
    return WIRE_INFO_OK;
}
wire_info_status_t wire_info_encode(const wire_info_t *db, unsigned message, wire_info_record_t *record,
    uint8_t *output, size_t capacity, size_t *used)
{ return execute(db,message,record,NULL,output,capacity,used,0); }
wire_info_status_t wire_info_decode(const wire_info_t *db, unsigned message, wire_info_record_t *record,
    const uint8_t *input, size_t length, size_t *used)
{ return execute(db,message,record,input,NULL,length,used,1); }
const char *wire_info_error(wire_info_status_t status)
{
    static const char *const names[]={"ok","argument","schema","range","truncated","capacity","constant","checksum","constraint"};
    return (unsigned)status<sizeof(names)/sizeof(names[0])?names[status]:"unknown";
}
