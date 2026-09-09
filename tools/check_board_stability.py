"""Bounded serial stability check. Optional voice exercise uses line-audio TX."""
import argparse
from pathlib import Path
import re
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--seconds', type=int, default=180)
    parser.add_argument('--output', type=Path, default=Path('build/board-stability.log'))
    parser.add_argument('--exercise-voice', action='store_true')
    parser.add_argument('--require-boot', action='store_true')
    args = parser.parse_args()
    if args.seconds < (90 if args.exercise_voice else 20):
        parser.error('Use at least 90 seconds for voice exercise, otherwise 20.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    schedule = [(8, 'STATUS'), (9, 'RXDIAG')]
    if args.exercise_voice:
        schedule += [(10, 'ROLE RX'), (12, 'CONTENT AUDIO'), (14, 'TXMODE 8PSK1200F'),
                     (16, 'ROLE TX'), (23, 'RXDIAG'), (24, 'STATUS'), (26, 'ROLE RX'),
                     (30, 'RXDIAG'), (32, 'CONTENT IMAGE'), (34, 'CONTENT TEXT'),
                     (36, 'MODE PSK125R'), (38, 'CONTENT AUDIO'), (40, 'TXMODE 8PSK1000F'),
                     (42, 'ROLE TX'), (49, 'RXDIAG'), (50, 'STATUS'), (52, 'ROLE RX'),
                     (56, 'RXDIAG'), (58, 'CONTENT TEXT'), (60, 'MODE PSK250R'),
                     (62, 'STATUS'), (64, 'MODE BPSK31')]
    schedule += [(n, command) for n in range(70 if args.exercise_voice else 15, args.seconds - 1, 20)
                 for command in ('STATUS', 'RXDIAG')]
    schedule.sort()
    port = serial.Serial(port=None, baudrate=115200, timeout=0.05, write_timeout=1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    received = bytearray()
    fault = re.compile(rb"Guru Meditation|panic'ed|assert failed|ESP_ERROR_CHECK failed|abort\(\) was called|Task watchdog got triggered|Interrupt wdt timeout|Brownout detector")
    started = time.monotonic()
    next_command = 0
    failure = None
    try:
        with args.output.open('wb') as log:
            while time.monotonic() - started < args.seconds:
                elapsed = time.monotonic() - started
                if next_command < len(schedule) and elapsed >= schedule[next_command][0]:
                    command = schedule[next_command][1]
                    port.write((command + '\n').encode())
                    print(f'{elapsed:.1f}s: {command}', flush=True)
                    next_command += 1
                data = port.read(8192)
                if data:
                    received.extend(data)
                    log.write(data)
                    log.flush()
                    if fault.search(received[-16384:]):
                        failure = 'Device panic/watchdog detected'
                        break
                    if elapsed > 10 and b'ESP-ROM:' in data:
                        failure = 'Unexpected device reboot during the observation window'
                        break
    finally:
        if args.exercise_voice:
            try:
                port.write(b'ROLE RX\n')
                port.flush()
            except serial.SerialException:
                pass
        port.close()
    text = received.decode('utf-8', errors='replace')
    checks = [('[status]' in text, 'Serial status response missing'),
              ('Codec2 ready=1' in text, 'Codec2 ready diagnostic missing'),
              ('stack free: audio=' in text, 'Stack diagnostic missing')]
    if args.require_boot:
        checks.append(('ESP32-P4 PSK modem started; main stack free=' in text, 'Completed startup missing'))
    if args.exercise_voice:
        encoded = [int(n) for n in re.findall(r'encode=(\d+)', text)]
        checks += [(sum(n > 0 for n in encoded) >= 2, 'Both voice encoding intervals were not observed'),
                   ('txmode=8PSK1200F' in text and 'txmode=8PSK1000F' in text, 'Both voice modes were not observed'),
                   ('psk=PSK250R' in text, 'Return to text mode was not observed')]
    problems = ([failure] if failure else []) + [message for ok, message in checks if not ok]
    for line in text.splitlines():
        if any(key in line for key in ('Codec2 ready=', 'stack free', '[status]', 'heap internal=', 'perf load=')):
            print(line)
    elapsed = time.monotonic() - started
    print(f'{"FAIL" if problems else "PASS"}: {elapsed:.1f}s; log: {args.output}')
    for message in problems:
        print(message)
    return bool(problems)


if __name__ == '__main__':
    raise SystemExit(main())
