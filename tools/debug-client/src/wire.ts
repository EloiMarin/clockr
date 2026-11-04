// A minimal protobuf (proto3) wire-format *writer* — the encoding counterpart
// of protobuf.ts's reader. The debug schema is tiny and owned by this repo, so
// a few varint/tag helpers cover it; there is no protobuf runtime dependency.
//
// Wire types used by the schema:
//
//   0 varint, 1 fixed64, 2 length-delimited (string/bytes/message), 5 fixed32
//
// Reference: https://protobuf.dev/programming-guides/encoding/

/** A value accepted wherever the wire format wants an integer. */
export type Int64 = bigint | number;

/** Unsigned LEB128 of a non-negative integer, as an array of bytes. */
export function varint(value: Int64): number[] {
  let v = BigInt(value);

  if (v < 0n) throw new RangeError("protobuf: varint values must be non-negative");

  const out: number[] = [];

  for (;;) {
    const byte = Number(v & 0x7fn);
    v >>= 7n;

    if (v === 0n) {
      out.push(byte);
      return out;
    }

    out.push(byte | 0x80);
  }
}

/** zigzag encoding for `sint64` (`int_value` in `Value`). */
export function zigzag(value: Int64): bigint {
  const n = BigInt.asIntN(64, BigInt(value));

  return (n << 1n) ^ (n >> 63n);
}

/** A field key: (field number << 3) | wire type. */
export function tag(field: number, wire: number): number[] {
  return varint((field << 3) | wire);
}

/** A length-delimited field (string, bytes, or a nested message). */
export function bytesField(field: number, bytes: Uint8Array | ArrayLike<number>): number[] {
  return [...tag(field, 2), ...varint(bytes.length), ...bytes];
}

/** A UTF-8 string field. */
export function stringField(field: number, text: string): number[] {
  return bytesField(field, new TextEncoder().encode(text));
}

/** A nested message field — the payload is the encoded message body. */
export function messageField(field: number, body: number[]): number[] {
  return bytesField(field, body);
}

/**
 * A `uint64`/`bool`/enum varint field. proto3 serializers omit fields whose
 * value is the type default; pass `omitDefault` for plain (non-oneof) fields
 * to mirror that. Oneof members are always encoded, because presence is
 * meaningful there — `Value.bool_value = false` must travel.
 */
export function varintField(
  field: number,
  value: Int64 | boolean,
  options: { omitDefault?: boolean } = {},
): number[] {
  const n = typeof value === "boolean" ? BigInt(value ? 1 : 0) : BigInt(value);

  if (options.omitDefault === true && n === 0n) return [];

  return [...tag(field, 0), ...varint(n)];
}

/** A `sint64` (zigzag) varint field. */
export function sint64Field(field: number, value: Int64): number[] {
  return [...tag(field, 0), ...varint(zigzag(value))];
}

/** A `double` field (fixed64, little-endian). */
export function doubleField(field: number, value: number): number[] {
  const bytes = new Uint8Array(8);
  new DataView(bytes.buffer).setFloat64(0, value, true);

  return [...tag(field, 1), ...bytes];
}

/** A packed run of varints (`repeated uint64`), as proto3 emits it. */
export function packedVarintField(field: number, values: Int64[]): number[] {
  return bytesField(field, values.flatMap((v) => varint(v)));
}

/** Flatten byte arrays into one `Uint8Array` (the frame to send). */
export function concatBytes(parts: Array<number[] | Uint8Array>): Uint8Array {
  let length = 0;

  for (const part of parts) length += part.length;

  const out = new Uint8Array(length);
  let offset = 0;

  for (const part of parts) {
    out.set(part, offset);
    offset += part.length;
  }

  return out;
}
