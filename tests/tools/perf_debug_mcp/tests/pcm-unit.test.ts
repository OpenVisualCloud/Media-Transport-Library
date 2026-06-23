/**
 * Unit tests for PCM bridge helpers and data extraction logic.
 */
import { describe, test, expect } from "@jest/globals";
import { num, sub } from "../src/collectors/pcm-bridge.js";

// ─── num() safe numeric accessor ───────────────────────────────────────────

describe("num()", () => {
  test("extracts existing numeric key", () => {
    expect(num({ "Instructions Retired Any": 1234567 }, "Instructions Retired Any")).toBe(1234567);
  });

  test("returns 0 for missing key", () => {
    expect(num({ a: 1 }, "b")).toBe(0);
  });

  test("returns 0 for undefined obj", () => {
    expect(num(undefined, "anything")).toBe(0);
  });

  test("returns 0 for non-numeric value", () => {
    expect(num({ a: "not a number" }, "a")).toBe(0);
  });

  test("returns 0 for NaN value", () => {
    expect(num({ a: NaN }, "a")).toBe(0);
  });

  test("handles zero correctly", () => {
    expect(num({ a: 0 }, "a")).toBe(0);
  });

  test("handles negative numbers", () => {
    expect(num({ a: -42 }, "a")).toBe(-42);
  });

  test("handles floating point", () => {
    expect(num({ "IPC": 2.31 }, "IPC")).toBeCloseTo(2.31);
  });
});

// ─── sub() safe sub-object accessor ────────────────────────────────────────

describe("sub()", () => {
  test("extracts existing sub-object", () => {
    const obj = { "Core Counters": { "IPC": 1.5 } };
    const result = sub(obj, "Core Counters");
    expect(result).toEqual({ "IPC": 1.5 });
  });

  test("returns undefined for missing key", () => {
    expect(sub({ a: 1 }, "missing")).toBeUndefined();
  });

  test("returns undefined for null obj", () => {
    expect(sub(null, "anything")).toBeUndefined();
  });

  test("returns undefined for undefined obj", () => {
    expect(sub(undefined, "anything")).toBeUndefined();
  });

  test("returns value for non-object value (no type guard)", () => {
    // sub() only guards against null/undefined obj, not the value type
    expect(sub({ a: 42 }, "a")).toBe(42);
  });

  test("returns array value (no type guard on value)", () => {
    expect(sub({ a: [1, 2, 3] }, "a")).toEqual([1, 2, 3]);
  });

  test("handles nested access", () => {
    const obj = {
      "Socket": {
        "Core": {
          "Thread": { value: 99 }
        }
      }
    };
    const socket = sub(obj, "Socket");
    expect(socket).toBeDefined();
    const core = sub(socket!, "Core");
    expect(core).toBeDefined();
    const thread = sub(core!, "Thread");
    expect(thread).toEqual({ value: 99 });
  });
});
