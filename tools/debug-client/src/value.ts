// The JSON-friendly mapping of `kernel::value` this client uses at its API
// boundary, plus the sugar that lets callers write plain JS scalars.
//
// Canonical mapping (what decoders return and encoders accept):
//
//   { kind: "unit" }                                  monostate / "no event"
//   { kind: "bool",   value: true }                   bool
//   { kind: "int",    value: 42n }                    int64 — bigint, no truncation
//   { kind: "double", value: 2.5 }                    double
//   { kind: "struct", value: { typeId, fields } }     a described user struct
//   { kind: "string", value: "saw" }                  text / enum spelling
//   { kind: "absent" }                                an unset oneof on the wire
//
// Encoders also accept the sugar forms below, because the common case should
// not need a discriminated union: `setInput("bpm", 120.0)`. Sugar is converted
// to the canonical mapping before hitting the wire:
//
//   boolean -> { kind: "bool", value }
//   bigint  -> { kind: "int", value }            (explicit: an int, not a double)
//   number  -> { kind: "double", value }         (112.0 is a double, never an int)
//   string  -> { kind: "string", value }
//   null / undefined -> { kind: "unit" }
//
// The asymmetry is deliberate: `int64` needs bigint to be lossless, so a plain
// number is always a double. Use `{ kind: "int", value: 42n }` (or 42n) for an
// integer.

import type { StructValue, Value } from "./types.ts";

/** What callers may pass as a value: the canonical mapping or the sugar forms. */
export type ValueInput = Value | boolean | number | bigint | string | null | undefined;

const KINDS = new Set(["unit", "bool", "int", "double", "struct", "string", "absent"]);

/**
 * Normalize a caller-supplied value into the canonical mapping. Throws a
 * `TypeError` for anything that cannot be a kernel value.
 */
export function toValue(input: ValueInput): Value {
  if (input === null || input === undefined) return { kind: "unit" };

  switch (typeof input) {
    case "boolean":
      return { kind: "bool", value: input };
    case "bigint":
      return { kind: "int", value: input };
    case "number":
      if (!Number.isFinite(input)) return { kind: "double", value: input };
      return { kind: "double", value: input };
    case "string":
      return { kind: "string", value: input };
    case "object":
      return normalizeValueObject(input as Value);
    default:
      throw new TypeError(`value: unsupported JS value of type ${typeof input}`);
  }
}

function normalizeValueObject(value: Value): Value {
  const kind = (value as { kind?: unknown }).kind;

  if (typeof kind !== "string" || !KINDS.has(kind)) {
    throw new TypeError(
      'value: expected a Value mapping like { kind: "double", value: 1.0 } ' +
        `or a plain JS scalar, got ${JSON.stringify(value)}`,
    );
  }

  switch (kind) {
    case "unit":
      return { kind: "unit" };
    case "absent":
      return { kind: "absent" };
    case "bool": {
      if (typeof value.value !== "boolean") throw new TypeError("value: bool needs a boolean `value`");
      return { kind: "bool", value: value.value };
    }
    case "int": {
      if (typeof value.value === "bigint") return { kind: "int", value: value.value };
      if (typeof value.value === "number" && Number.isInteger(value.value)) {
        return { kind: "int", value: BigInt(value.value) };
      }
      if (typeof value.value === "string") return { kind: "int", value: BigInt(value.value) };
      throw new TypeError("value: int needs a bigint (or an integer number/string) `value`");
    }
    case "double": {
      if (typeof value.value !== "number") throw new TypeError("value: double needs a number `value`");
      return { kind: "double", value: value.value };
    }
    case "string": {
      if (typeof value.value !== "string") throw new TypeError("value: string needs a string `value`");
      return { kind: "string", value: value.value };
    }
    case "struct": {
      const struct = value.value as StructValue | undefined;

      if (struct === null || typeof struct !== "object" || !Array.isArray(struct.fields)) {
        throw new TypeError('value: struct needs a `value: { typeId, fields }` payload');
      }

      return {
        kind: "struct",
        value: {
          typeId: Number(struct.typeId ?? 0),
          fields: struct.fields.map((field) => toValue(field)),
        },
      };
    }
    default:
      throw new TypeError(`value: unsupported Value kind "${kind}"`);
  }
}

/**
 * A JSON-safe clone of a decoded Value: `int` becomes a number when it fits
 * exactly in a JS number, else its decimal string. This is for the MCP/JSON
 * boundary (JSON.stringify refuses bigint); the client itself never converts.
 */
export function valueToJson(value: Value): unknown {
  switch (value.kind) {
    case "unit":
      return { kind: "unit" };
    case "absent":
      return { kind: "absent" };
    case "bool":
    case "double":
    case "string":
      return { kind: value.kind, value: value.value };
    case "int":
      return { kind: "int", value: bigintToJson(value.value) };
    case "struct":
      return {
        kind: "struct",
        value: {
          typeId: value.value.typeId,
          fields: value.value.fields.map((field) => valueToJson(field)),
        },
      };
    default:
      return value;
  }
}

/** bigint -> number if lossless, else decimal string (JSON-friendly). */
export function bigintToJson(value: bigint): number | string {
  return value >= BigInt(Number.MIN_SAFE_INTEGER) && value <= BigInt(Number.MAX_SAFE_INTEGER)
    ? Number(value)
    : value.toString();
}
