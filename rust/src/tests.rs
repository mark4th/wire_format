use crate::{Error, Param, Record, RecordList, WireFormat};

#[test]
fn encodes_dns_header() {
    let mut out = [0u8; 12];
    let params = [
        Param::from(0x1234u16),
        Param::from(0x0100u16),
        Param::from(1u16),
    ];
    let mut wf = WireFormat::new_encode(&mut out, &params);
    let len = wf.parse("%p1%w%p2%w%p3%w%{0}%w%{0}%w%{0}%w").unwrap();
    assert_eq!(len, 12);
    assert_eq!(
        wf.output(),
        &[0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0]
    );
}

#[test]
fn addresses_parameters_beyond_nine_with_braces() {
    let params = [
        Param::Int(1),
        Param::Int(2),
        Param::Int(3),
        Param::Int(4),
        Param::Int(5),
        Param::Int(6),
        Param::Int(7),
        Param::Int(8),
        Param::Int(9),
        Param::Int(10),
    ];
    let mut output = [0u8; 2];
    let mut wf = WireFormat::new_encode(&mut output, &params);
    assert_eq!(wf.parse("%p{10}%b%p1%b").unwrap(), 2);
    assert_eq!(output, [10, 1]);

    let mut output = [0u8; 1];
    let mut wf = WireFormat::new_encode(&mut output, &params);
    assert_eq!(wf.parse("%p{0}%b"), Err(Error::InvalidParam));
    assert_eq!(wf.parse("%p{17}%b"), Err(Error::InvalidParam));
}

#[test]
fn emits_raw_parameter() {
    let payload = [0xde, 0xad, 0xbe, 0xef];
    let mut out = [0u8; 6];
    let params = [Param::Raw(&payload), Param::from(payload.len())];
    let mut wf = WireFormat::new_encode(&mut out, &params);
    wf.parse("%{165}%b%p1%p2%r%{90}%b").unwrap();
    assert_eq!(wf.output(), &[0xa5, 0xde, 0xad, 0xbe, 0xef, 0x5a]);
}

#[test]
fn captures_remaining_input_as_slice() {
    let input = [0x12, 0x34, 0xaa, 0xbb, 0xcc];
    let mut wf = WireFormat::new_decode(&input, &[]);
    wf.parse("%S%Pa%{0}%R").unwrap();
    assert_eq!(wf.int_var(b'a'), Some(0x1234));
    assert_eq!(wf.slice(0), Some(&input[2..]));
    assert_eq!(wf.input_pos(), input.len());
}

#[test]
fn emits_version_two_slice_slot() {
    let bytes = [0xde, 0xad, 0xbe, 0xef];
    let mut output = [0u8; 8];
    let mut wf = WireFormat::new_encode(&mut output, &[]);
    wf.set_slice(2, &bytes).unwrap();
    wf.parse("%{2}%v").unwrap();
    assert_eq!(wf.output(), &bytes);
}

#[test]
fn encodes_and_decodes_sdnv_bounded_slices() {
    let bytes = [0xde, 0xad, 0xbe];
    let expected = [0x95, 0x3c, 0x03, 0xde, 0xad, 0xbe, 0x7f];
    let mut output = [0u8; 16];
    let params = [Param::from(0xabcu64)];
    let mut enc = WireFormat::new_encode(&mut output, &params);
    enc.set_slice(1, &bytes).unwrap();
    enc.parse("%p1%d%{1}%z%d%{1}%z%{1}%V%{127}%b").unwrap();
    assert_eq!(enc.output(), &expected);

    let mut dec = WireFormat::new_decode(&expected, &[]);
    dec.parse("%D%Pa%D%Pb%gb%{1}%N%B%Pc").unwrap();
    assert_eq!(dec.int_var(b'a'), Some(0xabc));
    assert_eq!(dec.int_var(b'b'), Some(3));
    assert_eq!(dec.slice(1), Some(bytes.as_slice()));
    assert_eq!(dec.int_var(b'c'), Some(0x7f));
}

#[test]
fn encodes_and_decodes_counted_records() {
    let encode_formats = ["%p1%b%p2%d"];
    let mut encode_rows = [Record::new(), Record::new()];
    encode_rows[0].set_int(0, 0x11).unwrap();
    encode_rows[0].set_int(1, 1).unwrap();
    encode_rows[1].set_int(0, 0x22).unwrap();
    encode_rows[1].set_int(1, 0xabc).unwrap();
    let encode_list = RecordList::new(&mut encode_rows, 2, 2).unwrap();
    let mut output = [0u8; 16];
    let mut enc = WireFormat::new_encode(&mut output, &[]);
    enc.set_formats(&encode_formats).unwrap();
    enc.set_record_list(0, encode_list).unwrap();
    enc.parse("%{0}%k%d%{0}%k%{0}%{2}%J[0]%{238}%b").unwrap();
    assert_eq!(enc.output(), &[2, 0x11, 1, 0x22, 0x95, 0x3c, 0xee]);

    let decode_formats = ["%B%Pa%D%Pb"];
    let mut decode_rows = [Record::new(), Record::new()];
    let decode_list = RecordList::for_decode(&mut decode_rows, 2).unwrap();
    let mut dec = WireFormat::new_decode(enc.output(), &[]);
    dec.set_formats(&decode_formats).unwrap();
    dec.set_record_list(0, decode_list).unwrap();
    dec.parse("%D%Pa%ga%{0}%{2}%J[0]%B%Pb").unwrap();
    let decoded = dec.record_list(0).unwrap();
    assert_eq!(decoded.count(), 2);
    assert_eq!(decoded.records()[0].int(0), Some(0x11));
    assert_eq!(decoded.records()[1].int(1), Some(0xabc));
    assert_eq!(dec.int_var(b'b'), Some(0xee));
}

#[test]
fn counted_records_can_nest_through_a_choice() {
    let formats = [
        "%p1%d%p2%d",
        "%p1%d%{1}%k%d%{1}%k%{1}%{2}%J[0]",
        "%p1%b%?%p1%{8}%=%t%{1}%{1}%{2}%J[1]%;",
    ];
    let mut claim_rows = [Record::new(), Record::new()];
    claim_rows[0].set_int(0, 0).unwrap();
    claim_rows[0].set_int(1, 5).unwrap();
    claim_rows[1].set_int(0, 5).unwrap();
    claim_rows[1].set_int(1, 7).unwrap();
    let claims = RecordList::new(&mut claim_rows, 2, 2).unwrap();
    let mut body_rows = [Record::new()];
    body_rows[0].set_int(0, 3).unwrap();
    body_rows[0].set_record_list(1, claims).unwrap();
    let body = RecordList::new(&mut body_rows, 1, 2).unwrap();
    let params = [Param::from(8u8)];
    let mut output = [0u8; 16];
    let mut enc = WireFormat::new_encode(&mut output, &params);
    enc.set_formats(&formats).unwrap();
    enc.set_record_list(1, body).unwrap();
    enc.parse(formats[2]).unwrap();
    assert_eq!(enc.output(), &[8, 3, 2, 0, 5, 5, 7]);
}

#[test]
fn emits_typed_arrays_in_big_endian_order() {
    let bytes = [0x11, 0x22, 0x33];
    let shorts = [0x1122, 0x3344, 0x5566];
    let longs = [0x1122_3344, 0x5566_7788];
    let params = [
        Param::Raw(&bytes),
        Param::from(bytes.len()),
        Param::U16s(&shorts),
        Param::from(shorts.len()),
        Param::U32s(&longs),
        Param::from(longs.len()),
    ];
    let mut out = [0u8; 17];
    let mut wf = WireFormat::new_encode(&mut out, &params);

    wf.parse("%p1%p2%r%p3%p4%r2%p5%p6%r4").unwrap();

    assert_eq!(
        wf.output(),
        &[
            0x11, 0x22, 0x33, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x11, 0x22, 0x33, 0x44, 0x55,
            0x66, 0x77, 0x88,
        ]
    );
}

#[test]
fn rejects_bad_array_size_and_array_type() {
    let bytes = [0x11, 0x22];
    let params = [Param::Raw(&bytes), Param::from(bytes.len())];
    let mut out = [0u8; 4];
    let mut wf = WireFormat::new_encode(&mut out, &params);

    assert_eq!(
        wf.parse("%p1%p2%r3"),
        Err(Error::InvalidArrayElementSize(3))
    );
    assert_eq!(wf.parse("%p1%p2%r2"), Err(Error::TypeMismatch));
}

#[test]
fn encodes_and_decodes_u64() {
    let params = [Param::from(0xffee_ddcc_bbaa_9988u64)];
    let mut out = [0u8; 8];
    let mut enc = WireFormat::new_encode(&mut out, &params);

    enc.parse("%p1%q").unwrap();
    assert_eq!(
        enc.output(),
        &[0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88]
    );

    let mut dec = WireFormat::new_decode(enc.output(), &[]);
    dec.parse("%Q%Pa").unwrap();
    assert_eq!(dec.int_var(b'a').unwrap() as u64, 0xffee_ddcc_bbaa_9988);
}

#[test]
fn fixed_width_emit_truncates_like_c() {
    let mut out = [0u8; 6];
    let params = [Param::from(0x12345u32), Param::from(0x1_2345_6789i64)];
    let mut wf = WireFormat::new_encode(&mut out, &params);
    wf.parse("%p1%w%p2%W").unwrap();
    assert_eq!(wf.output(), &[0x23, 0x45, 0x23, 0x45, 0x67, 0x89]);
}

#[test]
fn rejects_terminfo_decimal_width_prefixes() {
    let mut out = [0u8; 1];
    let params = [Param::from(7u8)];
    let mut wf = WireFormat::new_encode(&mut out, &params);
    assert_eq!(wf.parse("%p1%2b"), Err(Error::InvalidOperator(b'2')));
}

#[test]
fn upper_and_lowercase_variables_alias() {
    let mut out = [0u8; 1];
    let mut wf = WireFormat::new_encode(&mut out, &[]);
    wf.parse("%{42}%Pa%gA%b").unwrap();
    assert_eq!(wf.output(), &[42]);
}

#[test]
fn decodes_big_endian_values() {
    let input = [0x12, 0x34, 0xca, 0xfe, 0xba, 0xbe];
    let mut wf = WireFormat::new_decode(&input, &[]);
    wf.parse("%S%Pa%L%Pb").unwrap();
    assert_eq!(wf.int_var(b'a'), Some(0x1234));
    assert_eq!(wf.int_var(b'b'), Some(0xcafebabe));
    assert_eq!(wf.input_pos(), 6);
}

#[test]
fn encodes_and_decodes_bit_fields() {
    let mut out = [0u8; 1];
    let params = [Param::from(4u8), Param::from(5u8)];
    let mut enc = WireFormat::new_encode(&mut out, &params);
    enc.parse("%p1%{4}%{4}%x%p2%{4}%{0}%x%f").unwrap();
    assert_eq!(enc.output(), &[0x45]);

    let mut dec = WireFormat::new_decode(enc.output(), &[]);
    dec.parse("%{4}%{4}%X%Pa%{4}%{0}%X%Pb%f").unwrap();
    assert_eq!(dec.int_var(b'a'), Some(4));
    assert_eq!(dec.int_var(b'b'), Some(5));
}

#[test]
fn supports_conditionals() {
    let mut out = [0u8; 4];
    let params = [Param::from(0u8)];
    let mut wf = WireFormat::new_encode(&mut out, &params);
    wf.parse("%?%p1%t%'Y'%c%e%'N'%c%;").unwrap();
    assert_eq!(wf.output(), b"N");
}

#[test]
fn fault_operator_records_nonfatal_fault_and_continues() {
    let mut out = [0u8; 2];
    let mut wf = WireFormat::new_encode(&mut out, &[]);
    let len = wf.parse("%{1}%E%{66}%b").unwrap();
    assert_eq!(len, 1);
    assert_eq!(wf.output(), &[66]);
    assert_eq!(wf.faults(), 1);
}

#[test]
fn fault_operator_aborts_when_masked() {
    let mut out = [0u8; 2];
    let mut wf = WireFormat::new_encode(&mut out, &[]);
    wf.set_abort_mask(1);
    assert_eq!(wf.parse("%{1}%E%{66}%b"), Err(Error::FaultAbort(1)));
    assert_eq!(wf.output(), &[]);
    assert_eq!(wf.faults(), 1);
}

#[test]
fn faults_reset_each_parse_but_abort_mask_stays() {
    let mut out = [0u8; 2];
    let mut wf = WireFormat::new_encode(&mut out, &[]);
    wf.parse("%{2}%E").unwrap();
    assert_eq!(wf.faults(), 2);
    wf.set_abort_mask(4);
    wf.parse("%{65}%b").unwrap();
    assert_eq!(wf.faults(), 0);
    assert_eq!(wf.abort_mask(), 4);
    assert_eq!(wf.output(), &[65]);
}

#[test]
fn fault_operator_can_abort_decode_after_bad_magic() {
    let input = [0x00, 0x12, 0x34];
    let mut wf = WireFormat::new_decode(&input, &[]);
    wf.set_abort_mask(1);
    assert_eq!(
        wf.parse("%B%Pa%?%ga%{165}%=%t%S%Pb%e%{1}%E%;"),
        Err(Error::FaultAbort(1))
    );
    assert_eq!(wf.input_pos(), 1);
    assert_eq!(wf.int_var(b'a'), Some(0));
    assert_eq!(wf.faults(), 1);
}

#[test]
fn calls_and_repeats_formats() {
    let formats = ["%{1}%b", "%{2}%b%[0]"];
    let mut out = [0u8; 8];
    let mut wf = WireFormat::new_encode(&mut out, &[]);

    wf.set_formats(&formats).unwrap();
    let len = wf.parse("%{3}%:%[1]").unwrap();

    assert_eq!(len, 6);
    assert_eq!(wf.output(), &[2, 1, 2, 1, 2, 1]);
}

#[test]
fn format_call_returns_to_decode_caller() {
    let formats = ["%S%Pa%S%Pb"];
    let input = [0, 7, 0, 9, 0, 42];
    let mut wf = WireFormat::new_decode(&input, &[]);

    wf.set_formats(&formats).unwrap();
    wf.parse("%[0]%S%Pc").unwrap();

    assert_eq!(wf.int_var(b'a'), Some(7));
    assert_eq!(wf.int_var(b'b'), Some(9));
    assert_eq!(wf.int_var(b'c'), Some(42));
    assert_eq!(wf.input_pos(), input.len());
}

#[test]
fn format_calls_refuse_forward_and_self_references() {
    let formats = ["%{1}%b%[1]", "%{2}%b%[1]"];
    let mut out = [0u8; 4];
    let mut wf = WireFormat::new_encode(&mut out, &[]);

    wf.set_formats(&formats).unwrap();
    wf.parse("%[0]%[1]").unwrap();

    assert_eq!(wf.output(), &[1, 2]);
}

#[test]
fn validates_format_table_limits_and_depth() {
    let too_many = [""; crate::MAX_FORMATS + 1];
    let too_deep = ["", "%[0]", "%[1]", "%[2]", "%[3]", "%[4]", "%[5]", "%[6]"];
    let mut out = [0u8; 1];
    let mut wf = WireFormat::new_encode(&mut out, &[]);

    assert_eq!(wf.set_formats(&too_many), Err(Error::TooManyFormats));
    assert_eq!(wf.set_formats(&too_deep), Err(Error::FormatCallDepth));
}

#[test]
fn fault_inside_called_format_obeys_abort_mask() {
    let formats = ["%{4}%E%{88}%b"];
    let mut out = [0u8; 1];
    let mut wf = WireFormat::new_encode(&mut out, &[]);

    wf.set_formats(&formats).unwrap();
    wf.set_abort_mask(4);
    assert_eq!(wf.parse("%[0]"), Err(Error::FaultAbort(4)));
    assert_eq!(wf.output(), &[]);
    assert_eq!(wf.faults(), 4);
}
