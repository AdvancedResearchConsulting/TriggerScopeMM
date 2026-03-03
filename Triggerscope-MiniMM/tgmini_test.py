#!/usr/bin/env python3
"""
TriggerScope Mini — Board Validation Test
Emulates MicroManager initialization sequence then runs
a manual hardware verification with the operator.

Usage:
    python tgmini_test.py [COM_PORT]
    python tgmini_test.py COM25
    python tgmini_test.py /dev/ttyACM0
"""

import serial
import sys
import time
import serial.tools.list_ports

BAUD = 115200
TIMEOUT = 2.0  # seconds

def select_serial_port():
    ports = list(serial.tools.list_ports.comports())

    if not ports:
        print("No serial ports found.")
        return None

    print("\nAvailable Serial Ports:")
    for i, port in enumerate(ports):
        print(f"[{i}] {port.device} - {port.description}")

    while True:
        try:
            selection = int(input("\nSelect port number: "))
            if 0 <= selection < len(ports):
                return ports[selection].device
            else:
                print("Invalid selection. Try again.")
        except ValueError:
            print("Please enter a valid number.")

def send(ser, cmd):
    """Send a command and return the response (stripped)."""
    full = cmd + "\n"
    ser.write(full.encode())
    resp = ser.readline().decode().strip()
    return resp


def expect(ser, cmd, expected_prefix):
    """Send command, check response starts with expected prefix. Returns (ok, response)."""
    resp = send(ser, cmd)
    ok = resp.startswith(expected_prefix)
    return ok, resp


def run_test(port):
    print(f"\n{'='*60}")
    print(f"  TriggerScope Mini — Board Validation Test")
    print(f"{'='*60}\n")

    # ── Connect ──────────────────────────────────────────────
    print(f"[*] Opening {port} at {BAUD} baud...")
    try:
        ser = serial.Serial(port, BAUD, timeout=TIMEOUT)
    except serial.SerialException as e:
        print(f"[FAIL] Could not open {port}: {e}")
        return False

    # Flush any boot message
    time.sleep(2)
    ser.reset_input_buffer()

    # ════════════════════════════════════════════════════════════
    #  STEP 1 — Initialization (emulates MicroManager startup)
    # ════════════════════════════════════════════════════════════
    print("\n── Step 1: Initialization ──\n")
    all_pass = True

    # * — ID query
    ok, resp = expect(ser, "*", "ARC TRIGGERSCOPE")
    print(f"  [{'PASS' if ok else 'FAIL'}] ID query       -> {resp}")
    all_pass &= ok

    # SSL1 — signal LEDs on
    ok, resp = expect(ser, "SSL1", "!SSL")
    print(f"  [{'PASS' if ok else 'FAIL'}] SSL1           -> {resp}")
    all_pass &= ok

    # PDN0 — query digital sequence capacity
    ok, resp = expect(ser, "PDN0", "!PDN0-")
    print(f"  [{'PASS' if ok else 'FAIL'}] PDN0           -> {resp}")
    all_pass &= ok

    # SAR1-1 — set DAC 1 range 0-5V
    ok, resp = expect(ser, "SAR1-1", "!SAR1-1")
    print(f"  [{'PASS' if ok else 'FAIL'}] SAR1-1         -> {resp}")
    all_pass &= ok

    # PAN1 — query DAC 1 sequence capacity
    ok, resp = expect(ser, "PAN1", "!PAN1-")
    print(f"  [{'PASS' if ok else 'FAIL'}] PAN1           -> {resp}")
    all_pass &= ok

    # SAR2-1 — set DAC 2 range 0-5V
    ok, resp = expect(ser, "SAR2-1", "!SAR2-1")
    print(f"  [{'PASS' if ok else 'FAIL'}] SAR2-1         -> {resp}")
    all_pass &= ok

    # PAN2 — query DAC 2 sequence capacity
    ok, resp = expect(ser, "PAN2", "!PAN2-")
    print(f"  [{'PASS' if ok else 'FAIL'}] PAN2           -> {resp}")
    all_pass &= ok

    if all_pass:
        print("\n  *** INITIALIZATION: PASS ***")
    else:
        print("\n  *** INITIALIZATION: FAIL ***")
        ser.close()
        return False

    # ════════════════════════════════════════════════════════════
    #  STEP 2 — All outputs ON (DAC 1&2 full scale, TTL 1-4 high)
    # ════════════════════════════════════════════════════════════
    print("\n── Step 2: All Outputs ON ──\n")

    # DAC 1 → full scale (4095 = 5V in non-TS16 mode)
    ok, resp = expect(ser, "SAO1-4095", "!SAO1-4095")
    print(f"  [{'PASS' if ok else 'FAIL'}] DAC1 = 5.0V    -> {resp}")
    all_pass &= ok

    # DAC 2 → full scale
    ok, resp = expect(ser, "SAO2-4095", "!SAO2-4095")
    print(f"  [{'PASS' if ok else 'FAIL'}] DAC2 = 5.0V    -> {resp}")
    all_pass &= ok

    # TTL 1-4 all ON (bitmask 0x0F = 15)
    ok, resp = expect(ser, "SDO0-15", "!SDO0-15")
    print(f"  [{'PASS' if ok else 'FAIL'}] TTL 1-4 = ON   -> {resp}")
    all_pass &= ok

    print("\n  Verify with multimeter / oscilloscope:")
    print("    - DAC 1 (Channel B) output ≈ 5.0V")
    print("    - DAC 2 (Channel A) output ≈ 5.0V")
    print("    - TTL 1, 2, 3, 4 outputs  ≈ 5.0V")
    input("\n  Press ENTER when verified...")

    # ════════════════════════════════════════════════════════════
    #  STEP 3 — Blanking test (all outputs blank on TRIG1 input)
    # ════════════════════════════════════════════════════════════
    print("\n── Step 3: Blanking Test ──\n")

    # BAO — enable blanking on DAC 1: blank on low (t=0)
    ok, resp = expect(ser, "BAO1-1-0", "!BAO1-1-0")
    print(f"  [{'PASS' if ok else 'FAIL'}] DAC1 blanking  -> {resp}")
    all_pass &= ok

    # BAO — enable blanking on DAC 2: blank on low (t=0)
    ok, resp = expect(ser, "BAO2-1-0", "!BAO2-1-0")
    print(f"  [{'PASS' if ok else 'FAIL'}] DAC2 blanking  -> {resp}")
    all_pass &= ok

    # BDO — enable blanking on TTL group 0: blank on low (t=0)
    ok, resp = expect(ser, "BDO0-1-0", "!BDO0-1-0")
    print(f"  [{'PASS' if ok else 'FAIL'}] TTL blanking   -> {resp}")
    all_pass &= ok

    print("\n  Blanking is now active on all outputs.")
    print("  Apply a TTL signal to TRIG1 input and verify:")
    print("    - TRIG HIGH → all outputs ON  (DAC ≈ 5V, TTL HIGH)")
    print("    - TRIG LOW  → all outputs OFF (DAC ≈ 0V, TTL LOW)")
    input("\n  Press ENTER when verified...")

    # ════════════════════════════════════════════════════════════
    #  Cleanup — disable blanking, zero all outputs
    # ════════════════════════════════════════════════════════════
    print("\n── Cleanup ──\n")

    send(ser, "BAO1-0-0")
    send(ser, "BAO2-0-0")
    send(ser, "BDO0-0-0")
    send(ser, "SAO1-0")
    send(ser, "SAO2-0")
    send(ser, "SDO0-0")
    print("  All outputs zeroed, blanking disabled.")

    # ════════════════════════════════════════════════════════════
    #  Result
    # ════════════════════════════════════════════════════════════
    ser.close()

    print(f"\n{'='*60}")
    if all_pass:
        print("  RESULT: ALL TESTS PASSED")
    else:
        print("  RESULT: ONE OR MORE TESTS FAILED")
    print(f"{'='*60}\n")

    return all_pass


if __name__ == "__main__":
    p = select_serial_port()
    print('port is:', p)
    passed = run_test(p)
    sys.exit(0 if passed else 1)
