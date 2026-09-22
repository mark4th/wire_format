use crate::{Error, Param, WireFormat};

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
fn emits_raw_parameter() {
    let payload = [0xde, 0xad, 0xbe, 0xef];
    let mut out = [0u8; 6];
    let params = [Param::Raw(&payload), Param::from(payload.len())];
    let mut wf = WireFormat::new_encode(&mut out, &params);
    wf.parse("%{165}%b%p1%p2%r%{90}%b").unwrap();
    assert_eq!(wf.output(), &[0xa5, 0xde, 0xad, 0xbe, 0xef, 0x5a]);
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
