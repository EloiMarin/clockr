// A minimal protobuf (proto3) wire-format reader.
//
// The debugger's schema is small and owned by this repo, and the page is
// deliberately dependency-free (no bundler, no protobufjs runtime). This
// reader understands the wire types the schema uses:
//
//   0 varint, 1 fixed64, 2 length-delimited, 5 fixed32
//
// Decoded messages are plain objects keyed by field number: { 1: 123, 2: [..] }
// with repeated fields always arrays. schema.js maps those numbers to names.
// Field numbers and types here mirror proto/clockr_debug.proto; when the
// schema changes, update both.
//
// Reference: https://protobuf.dev/programming-guides/encoding/

class Reader {
  constructor(bytes, pos = 0, end = bytes.length) {
    this.bytes = bytes;
    this.pos = pos;
    this.end = end;
  }

  get done() {
    return this.pos >= this.end;
  }

  varint() {
    let result = 0n;
    let shift = 0n;
    for (;;) {
      if (this.pos >= this.end) throw new Error("protobuf: truncated varint");
      const byte = this.bytes[this.pos++];
      result |= BigInt(byte & 0x7f) << shift;
      if ((byte & 0x80) === 0) break;
      shift += 7n;
    }
    return result;
  }

  // Length-delimited field payload as a sub-reader.
  sub() {
    const length = Number(this.varint());
    if (this.pos + length > this.end) throw new Error("protobuf: truncated length-delimited field");
    const sub = new Reader(this.bytes, this.pos, this.pos + length);
    this.pos += length;
    return sub;
  }

  bytes_field() {
    const sub = this.sub();
    return this.bytes.subarray(sub.pos, sub.end);
  }

  string() {
    return new TextDecoder().decode(this.bytes_field());
  }

  // tag -> {field, wire}
  tag() {
    const key = Number(this.varint());
    return { field: key >>> 3, wire: key & 0x7 };
  }

  skip(wire) {
    switch (wire) {
      case 0: this.varint(); break;
      case 1: this.pos += 8; break;
      case 2: this.sub(); break;
      case 5: this.pos += 4; break;
      default: throw new Error("protobuf: unsupported wire type " + wire);
    }
  }

  // Read one scalar of the given wire type into a JS value. fixed64 is handed
// back as a raw DataView (its bits may be a double or a uint64 depending on
// the field), and fixed32 as a uint32.
  scalar(wire) {
    switch (wire) {
      case 0: return this.varint();
      case 1: {
        const view = new DataView(this.bytes.buffer, this.bytes.byteOffset + this.pos, 8);
        this.pos += 8;
        return { __fixed64: view };
      }
      case 5: {
        const view = new DataView(this.bytes.buffer, this.bytes.byteOffset + this.pos, 4);
        this.pos += 4;
        return { __fixed32: view };
      }
      default: throw new Error("protobuf: scalar with wire type " + wire);
    }
  }
}

// Read a message body into a { fieldNumber: value } map. Repeated fields
// accumulate into arrays; a field is never both repeated and singular in a
// given schema, so the reader does not need to know which it is.
export function decodeMessage(reader) {
  const out = {};
  while (!reader.done) {
    const { field, wire } = reader.tag();
    let value;
    if (wire === 2) {
      // Ambiguous between string/bytes and a nested message. Callers that
      // know the type re-interpret; here we hand back the raw sub-reader so
      // schema.js can decide without a second pass.
      const sub = reader.sub();
      value = { __sub: sub };
    } else {
      value = reader.scalar(wire);
    }
    if (out[field] === undefined) out[field] = value;
    else if (Array.isArray(out[field])) out[field].push(value);
    else out[field] = [out[field], value];
  }
  return out;
}

export { Reader };

// Helpers used by schema.js to interpret a length-delimited slot.

export function asSubReader(slot) {
  if (slot && slot.__sub) return slot.__sub;
  throw new Error("protobuf: expected a length-delimited field");
}

export function asString(slot) {
  return new TextDecoder().decode(asBytes(slot));
}

export function asBytes(slot) {
  const sub = asSubReader(slot);
  return sub.bytes.subarray(sub.pos, sub.end);
}

// zigzag decode for sint64 (used by Value.int_value).
export function zigzag64(value) {
  const n = BigInt.asIntN(64, BigInt(value));
  return Number((n >> 1n) ^ -(n & 1n));
}

export function toNumber(value) {
  if (typeof value === "bigint") return Number(value);
  return value;
}

// Decode a length-delimited slot as a sequence of unsigned varints — proto3
// encodes repeated scalar fields (uint64, sint64, bool, enum, ...) as a
// single packed field by default, so a "repeated uint64 fan_in = 6" arrives
// as one wire-2 field whose payload is varint after varint.
export function decodePackedVarints(slot) {
  const sub = asSubReader(slot);
  const out = [];
  while (!sub.done) out.push(Number(sub.varint()));
  return out;
}