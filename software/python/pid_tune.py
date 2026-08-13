"""
Host side of the knob's haptic tuning loop.

The firmware owns the controller (firmware/src/haptic_pid.h) and the gain
schedule that feeds it (firmware/src/haptic_schedule.h). This tool exists because
the one thing the knob cannot do is tell you what it just felt like: the settings
rows change the gains live, but judging the result by hand means holding a
memory of the previous setting while your fingers are already on the next one.

So it does three things and nothing else:

    capture   record the trace stream to a CSV
    analyse   turn a capture into settling numbers you can compare
    sweep     walk one term through a range, capturing and analysing each step

Speaks the same plaintext lines as smartknob_deck, so it needs only pyserial and
does not switch the knob out of the protocol it boots into:

    py software/python/pid_tune.py sweep --term d --values 60,100,140,180
    py software/python/pid_tune.py capture --seconds 20 --out spin.csv
    py software/python/pid_tune.py analyse spin.csv

Turn the knob while it captures. A capture with nothing moving in it has nothing
to measure, and the tool will say so rather than print zeros.
"""

import argparse
import csv
import math
import statistics
import sys
import time

import serial
import serial.tools.list_ports

SMARTKNOB_BAUD = 921600
READ_TIMEOUT_SECONDS = 0.05

# Duplicated from smartknob_deck rather than imported: that package pulls in the
# Windows volume and brightness backends at import time, and none of them have
# anything to do with tuning haptics.
KNOWN_USB_IDS = [
    (0x1a86, 0x7523),  # CH340
    (0x10c4, 0xea60),  # CP210x
]
ESPRESSIF_VID = 0x303a

# How long after a detent crossing we keep looking for the knob to settle. Past
# this the user has almost always moved on to the next detent, and what we would
# be measuring is the next move rather than this one's tail.
SETTLE_WINDOW_MS = 250

# Settled means "inside this fraction of the step that started it", held to the
# end of the window. A pure absolute threshold would call every fine detent
# settled instantly and every coarse one never.
SETTLE_FRACTION = 0.12

# Crossings whose step is smaller than this are noise around a detent the knob
# never really left, not a move worth scoring.
MIN_STEP_RADIANS = math.radians(0.4)


def candidate_ports():
    ports = sorted(serial.tools.list_ports.comports(), key=lambda p: p.device)
    return [p for p in ports
            if (p.vid, p.pid) in KNOWN_USB_IDS or p.vid == ESPRESSIF_VID]


def find_port():
    candidates = candidate_ports()
    if len(candidates) == 1:
        return candidates[0].device
    if not candidates:
        raise SystemExit('No SmartKnob-like serial device found; pass --port.')
    raise SystemExit(
        'Multiple SmartKnob-like devices found ({}); pass --port to choose one.'
        .format(', '.join(p.device for p in candidates)))


class Trace:
    """A capture: the column names the firmware sent, and the rows under them."""

    def __init__(self, columns, rows):
        self.columns = columns
        self.rows = rows

    def column(self, name):
        return [row[name] for row in self.rows]


def _parse_header(line):
    # "TRACE_HEADER: ms,pos,sub,..." - the firmware sends this once as the stream
    # starts, so the column order lives in one place and this tool has no copy of
    # it that can drift out of step.
    return [c.strip() for c in line.split(':', 1)[1].split(',')]


def _parse_sample(line, columns):
    fields = line.split(':', 1)[1].split(',')
    if len(fields) != len(columns):
        return None
    row = {}
    for name, text in zip(columns, fields):
        text = text.strip()
        try:
            row[name] = int(text) if name in ('ms', 'pos', 'flags') else float(text)
        except ValueError:
            return None
    return row


def capture(port, seconds, gains=None, echo=False):
    """Streams the trace for a while and returns it. Leaves tracing off."""
    columns = None
    rows = []
    with serial.Serial(port, SMARTKNOB_BAUD, timeout=READ_TIMEOUT_SECONDS) as ser:
        if gains is not None:
            ser.write('@PID {} {} {}\n'.format(*gains).encode('ascii'))
            # The motor task drops its integral on a gain change, so give the
            # knob a moment to settle before we start believing what it reports.
            time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(b'@TRACE 1\n')

        deadline = time.monotonic() + seconds
        buffer = bytearray()
        try:
            while time.monotonic() < deadline:
                chunk = ser.read(4096)
                if not chunk:
                    continue
                buffer.extend(chunk)
                *lines, rest = buffer.split(b'\n')
                buffer = bytearray(rest)
                for raw in lines:
                    line = raw.decode('utf-8', errors='replace').strip()
                    if line.startswith('TRACE_HEADER:'):
                        columns = _parse_header(line)
                    elif line.startswith('TRACE:') and columns:
                        row = _parse_sample(line, columns)
                        if row is not None:
                            rows.append(row)
                    elif echo and line:
                        print(line)
        except KeyboardInterrupt:
            pass
        finally:
            ser.write(b'@TRACE 0\n')
            ser.flush()

    if columns is None:
        raise SystemExit(
            'No TRACE_HEADER arrived. The knob only streams over the plaintext '
            'protocol - if something else has it in protobuf mode, unplug it.')
    return Trace(columns, rows)


def write_csv(trace, path):
    with open(path, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=trace.columns)
        writer.writeheader()
        writer.writerows(trace.rows)


def read_csv(path):
    with open(path, newline='', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        columns = list(reader.fieldnames or [])
        rows = []
        for raw in reader:
            row = {}
            for name, text in raw.items():
                row[name] = int(text) if name in ('ms', 'pos', 'flags') else float(text)
            rows.append(row)
    if not columns:
        raise SystemExit(f'{path} has no columns.')
    return Trace(columns, rows)


def _crossings(rows):
    """Indices where the reported position changed."""
    return [i for i in range(1, len(rows)) if rows[i]['pos'] != rows[i - 1]['pos']]


def _score_crossing(rows, start, stop):
    """
    Measures one detent crossing: how far the error was thrown, whether it came
    back past zero, and how long it took to stop moving.

    Ends at `stop`, the next crossing, as well as at the window length. Running
    past it would read the next detent's opening error - a full-sized step - as
    this one failing to settle, which makes every crossing score the same.
    """
    t0 = rows[start]['ms']
    window = [r for r in rows[start:stop] if r['ms'] - t0 <= SETTLE_WINDOW_MS]
    if len(window) < 4:
        return None

    step = abs(window[0]['err'])
    if step < MIN_STEP_RADIANS:
        return None

    sign = 1.0 if window[0]['err'] >= 0 else -1.0
    band = step * SETTLE_FRACTION

    # Overshoot is the error going past centre and out the other side, as a
    # percentage of the step that started it.
    overshoot = max(0.0, max(-sign * r['err'] for r in window)) / step * 100.0

    # Ringing: how many times the error changed sign. One crossing of zero is
    # what settling looks like; several is the knob bouncing.
    crossings = 0
    previous = sign
    for r in window:
        current = 1.0 if r['err'] >= 0 else -1.0
        if current != previous:
            crossings += 1
            previous = current

    # Settling time: the last moment the error was outside the band. Read from
    # the end backwards, so a late wobble counts and an early one does not.
    settle_ms = 0
    for r in reversed(window):
        if abs(r['err']) > band:
            settle_ms = r['ms'] - t0
            break

    return {
        'step_deg': math.degrees(step),
        'overshoot_pct': overshoot,
        'zero_crossings': crossings,
        'settle_ms': settle_ms,
        'peak_torque': max(abs(r['out']) for r in window),
    }


def analyse(trace):
    rows = trace.rows
    if not rows:
        raise SystemExit('Capture is empty.')

    starts = _crossings(rows)
    # Each crossing is scored up to the next one; the last runs to the end.
    stops = starts[1:] + [len(rows)]
    scored = [s for s in (_score_crossing(rows, i, stop)
                          for i, stop in zip(starts, stops))
              if s is not None]

    saturated = sum(1 for r in rows if r['flags'] & 1)
    dt_rejected = sum(1 for r in rows if r['flags'] & 2)

    # Motor noise while the knob is sitting still: the term that usually goes
    # wrong first when damping is pushed up, and the one that is audible before
    # it is visible in anything else.
    at_rest = [r for r in rows if abs(r['vel']) < 0.5]
    rest_torque_rms = 0.0
    if len(at_rest) > 1:
        rest_torque_rms = math.sqrt(
            sum(r['out'] ** 2 for r in at_rest) / len(at_rest))

    report = {
        'samples': len(rows),
        'duration_s': (rows[-1]['ms'] - rows[0]['ms']) / 1000.0,
        'detents': len(scored),
        'saturated_pct': saturated / len(rows) * 100.0,
        'dt_rejected_pct': dt_rejected / len(rows) * 100.0,
        'rest_torque_rms': rest_torque_rms,
        'gains': (rows[-1]['gp'], rows[-1]['gi'], rows[-1]['gd']),
    }
    if scored:
        report.update({
            'overshoot_pct': statistics.median(s['overshoot_pct'] for s in scored),
            'settle_ms': statistics.median(s['settle_ms'] for s in scored),
            'zero_crossings': statistics.median(s['zero_crossings'] for s in scored),
            'peak_torque': max(s['peak_torque'] for s in scored),
        })
    return report


def print_report(report):
    print(f"  samples          {report['samples']} over {report['duration_s']:.1f}s")
    print(f"  detents measured {report['detents']}")
    if report['detents'] == 0:
        print('  (nothing to score - turn the knob while it captures)')
    else:
        print(f"  overshoot        {report['overshoot_pct']:.1f}% (median)")
        print(f"  settling         {report['settle_ms']:.0f}ms (median)")
        print(f"  zero crossings   {report['zero_crossings']:.1f} (median)")
        print(f"  peak torque      {report['peak_torque']:.2f}")
    print(f"  torque at rest   {report['rest_torque_rms']:.3f} rms")
    print(f"  clamped          {report['saturated_pct']:.1f}% of samples")
    print(f"  dt rejected      {report['dt_rejected_pct']:.1f}% of samples")
    gp, gi, gd = report['gains']
    print(f"  gains in force   P={gp:.2f} I={gi:.2f} D={gd:.2f}")


def cmd_capture(args):
    port = args.port or find_port()
    print(f'Capturing {args.seconds}s from {port} - turn the knob.')
    trace = capture(port, args.seconds, echo=args.echo)
    print(f'{len(trace.rows)} samples.')
    if args.out:
        write_csv(trace, args.out)
        print(f'Wrote {args.out}')
    print_report(analyse(trace))


def cmd_analyse(args):
    print_report(analyse(read_csv(args.path)))


def cmd_sweep(args):
    port = args.port or find_port()
    values = [int(v) for v in args.values.split(',')]
    index = {'p': 0, 'i': 1, 'd': 2}[args.term]

    results = []
    for value in values:
        gains = [args.p, args.i, args.d]
        gains[index] = value
        print(f'\n{args.term.upper()}={value}%  '
              f'(P={gains[0]} I={gains[1]} D={gains[2]}) - '
              f'turn the knob for {args.seconds}s')
        trace = capture(port, args.seconds, gains=gains)
        report = analyse(trace)
        print_report(report)
        if args.out_prefix:
            path = f'{args.out_prefix}_{args.term}{value}.csv'
            write_csv(trace, path)
            print(f'  wrote {path}')
        results.append((value, report))

    print('\n' + '=' * 62)
    print(f'{args.term.upper():>5}  {"detents":>8}  {"overshoot":>10}  '
          f'{"settle":>8}  {"rings":>6}  {"rest":>7}')
    for value, report in results:
        if report['detents'] == 0:
            print(f'{value:>5}  {"0":>8}  {"-":>10}  {"-":>8}  {"-":>6}  '
                  f"{report['rest_torque_rms']:>7.3f}")
            continue
        print(f"{value:>5}  {report['detents']:>8}  "
              f"{report['overshoot_pct']:>9.1f}%  "
              f"{report['settle_ms']:>7.0f}ms  "
              f"{report['zero_crossings']:>6.1f}  "
              f"{report['rest_torque_rms']:>7.3f}")
    print('\nLower overshoot and settling is tighter; rising "rest" torque is the '
          'motor starting to buzz.')

    # The knob keeps whatever the last step set, which is rarely the one you
    # wanted. Put it back unless asked not to.
    if not args.keep:
        with serial.Serial(port, SMARTKNOB_BAUD, timeout=READ_TIMEOUT_SECONDS) as ser:
            ser.write(f'@PID {args.p} {args.i} {args.d}\n'.encode('ascii'))
        print(f'Restored P={args.p} I={args.i} D={args.d}.')


def cmd_set(args):
    port = args.port or find_port()
    with serial.Serial(port, SMARTKNOB_BAUD, timeout=READ_TIMEOUT_SECONDS) as ser:
        ser.write(f'@PID {args.p} {args.i} {args.d}\n'.encode('ascii'))
    print(f'Set P={args.p}% I={args.i}% D={args.d}%.')


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', help='Serial port; autodetected if omitted.')
    subparsers = parser.add_subparsers(dest='command', required=True)

    p = subparsers.add_parser('capture', help='Record the trace stream.')
    p.add_argument('--seconds', type=float, default=15.0)
    p.add_argument('--out', help='Write the samples to this CSV.')
    p.add_argument('--echo', action='store_true',
                   help='Print the knob\'s other serial output too.')
    p.set_defaults(func=cmd_capture)

    p = subparsers.add_parser('analyse', help='Score a capture already on disk.')
    p.add_argument('path')
    p.set_defaults(func=cmd_analyse)

    p = subparsers.add_parser('set', help='Set the three multipliers, in percent.')
    p.add_argument('p', type=int)
    p.add_argument('i', type=int)
    p.add_argument('d', type=int)
    p.set_defaults(func=cmd_set)

    p = subparsers.add_parser(
        'sweep', help='Walk one term through a range, scoring each step.')
    p.add_argument('--term', choices=['p', 'i', 'd'], default='d')
    p.add_argument('--values', default='60,100,140,180',
                   help='Comma-separated percentages for the swept term.')
    p.add_argument('--seconds', type=float, default=12.0)
    p.add_argument('--p', type=int, default=100, help='Held while sweeping.')
    p.add_argument('--i', type=int, default=0, help='Held while sweeping.')
    p.add_argument('--d', type=int, default=100, help='Held while sweeping.')
    p.add_argument('--out-prefix', help='Write each step to <prefix>_<term><n>.csv')
    p.add_argument('--keep', action='store_true',
                   help='Leave the last swept value in place when done.')
    p.set_defaults(func=cmd_sweep)

    args = parser.parse_args(argv)
    return args.func(args) or 0


if __name__ == '__main__':
    sys.exit(main())
