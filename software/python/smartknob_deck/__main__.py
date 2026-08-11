"""
Command line entry point: connect to a SmartKnob over USB and let it drive this
computer's volume and brightness.

The knob owns its menu and haptics; this process only applies the values it
sends. It speaks the firmware's plaintext host-agent lines, so it needs nothing
but pyserial and the OS backends in controls.py - no protobuf, and no switching
the knob out of the protocol it boots into.
"""

import argparse
import logging
import logging.handlers
import math
import os
import sys
import time

import serial
import serial.tools.list_ports

from . import instance
from .controller import DeckAgent
from .controls import (
    ALL_CONTROLS,
    ControlUnavailable,
    build_controls,
)

logger = logging.getLogger('smartknob_deck')

SMARTKNOB_BAUD = 921600

DEFAULT_CONTROLS = ['volume', 'brightness']

# USB-serial chips used by the SmartKnob View (CH340), the NanoFOC and other
# ESP32-S3 boards (native USB / CP210x)
KNOWN_USB_IDS = [
    (0x1a86, 0x7523),  # CH340
    (0x10c4, 0xea60),  # CP210x
]
ESPRESSIF_VID = 0x303a

RECONNECT_DELAY_SECONDS = 2.0

# Long enough to be cheap, short enough that a knob turn feels immediate.
READ_TIMEOUT_SECONDS = 0.05

LOG_MAX_BYTES = 256 * 1024


def default_log_path():
    """Where a hidden background run leaves its log."""
    base = os.environ.get('LOCALAPPDATA') or os.path.expanduser('~')
    return os.path.join(base, 'SmartKnob', 'smartknob_deck.log')


def configure_logging(verbose, log_path):
    handlers = []
    if log_path:
        try:
            os.makedirs(os.path.dirname(log_path), exist_ok=True)
            handlers.append(logging.handlers.RotatingFileHandler(
                log_path, maxBytes=LOG_MAX_BYTES, backupCount=1,
                encoding='utf-8'))
        except OSError as e:
            print(f'Could not open log file {log_path}: {e}', file=sys.stderr)
    # pythonw has no console, so a stream handler there would be a dead end.
    if sys.stderr is not None:
        handlers.append(logging.StreamHandler())

    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format='%(asctime)s %(levelname)-7s %(message)s',
        datefmt='%Y-%m-%d %H:%M:%S',
        handlers=handlers,
    )


def candidate_ports():
    ports = sorted(serial.tools.list_ports.comports(), key=lambda p: p.device)
    return [p for p in ports
            if (p.vid, p.pid) in KNOWN_USB_IDS or p.vid == ESPRESSIF_VID]


def find_port():
    """Pick the knob's port, or None if we can't tell which one it is."""
    candidates = candidate_ports()
    if len(candidates) == 1:
        port = candidates[0]
        logger.info('Using %s (%s)', port.device, port.description)
        return port.device
    if candidates:
        logger.warning('Multiple SmartKnob-like devices found (%s); '
                       'pass --port to choose one',
                       ', '.join(p.device for p in candidates))
    return None


def run_once(port, controls, poll_interval):
    """
    Connect and run until the knob goes away or the user interrupts.

    Returns True if it's worth reconnecting, False if we should stop.
    """
    with serial.Serial(port, SMARTKNOB_BAUD, timeout=READ_TIMEOUT_SECONDS) as ser:
        def send(verb, channel, value=None):
            line = f'@{verb} {channel}' if value is None \
                else f'@{verb} {channel} {value}'
            ser.write((line + '\n').encode('ascii'))

        agent = DeckAgent(controls, send, poll_interval=poll_interval)
        agent.start()

        buffer = bytearray()
        try:
            while True:
                chunk = ser.read(256)
                if chunk:
                    buffer.extend(chunk)
                    # Keep the tail: it may be a line still being transmitted.
                    *lines, rest = buffer.split(b'\n')
                    buffer = bytearray(rest)
                    for raw in lines:
                        _handle_raw(agent, raw)
                agent.tick()
        except KeyboardInterrupt:
            return False


def _handle_raw(agent, raw):
    try:
        line = raw.decode('utf-8', errors='replace').strip()
    except UnicodeDecodeError:
        return
    if not line:
        return
    if line.startswith('@'):
        agent.handle_line(line)
    else:
        # Everything else is the knob's own logging.
        logger.debug('knob: %s', line)


def _self_test(keys):
    """Report what each requested control can do here, without needing a knob."""
    ok = True
    for key in keys:
        factory = ALL_CONTROLS.get(key)
        if factory is None:
            print(f'{key}: unknown control')
            ok = False
            continue
        control = factory()
        try:
            value = control.read()
        except ControlUnavailable as e:
            print(f'{control.label}: unavailable - {e}')
            ok = False
            continue
        span = control.max_value - control.min_value
        step = math.degrees(control.position_width_radians)
        print(f'{control.label}: {value} '
              f'(range {control.min_value}-{control.max_value}, '
              f'{span + 1} detents, {step:.2f}\N{DEGREE SIGN} each, '
              f'{span * step:.0f}\N{DEGREE SIGN} end to end)')
    return 0 if ok else 1


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog='python -m smartknob_deck',
        description='Let a SmartKnob control this computer\'s volume and '
                    'brightness. The knob\'s own menu chooses what to adjust.')
    parser.add_argument('--port', help='serial port of the SmartKnob '
                                       '(auto-detected if omitted)')
    parser.add_argument('--controls', default=','.join(DEFAULT_CONTROLS),
                        help='comma-separated controls to serve. '
                             f'Available: {", ".join(sorted(ALL_CONTROLS))} '
                             '(default: %(default)s)')
    parser.add_argument('--poll-interval', type=float, default=2.0,
                        help='seconds between checks for changes made outside '
                             'the knob (default: %(default)s)')
    parser.add_argument('--no-reconnect', action='store_true',
                        help='exit instead of waiting for the knob to come back')
    parser.add_argument('--list-ports', action='store_true',
                        help='list serial ports that look like a SmartKnob and exit')
    parser.add_argument('--self-test', action='store_true',
                        help='report what each control reads right now and exit '
                             '(no knob required)')
    parser.add_argument('--stop', action='store_true',
                        help='stop the running agent and exit, freeing the '
                             'serial port so the firmware can be flashed')
    parser.add_argument('--restart', action='store_true',
                        help='stop the running agent and start a fresh hidden one')
    parser.add_argument('--allow-multiple', action='store_true',
                        help='skip the single-instance guard (two agents will '
                             'fight over the port; for debugging only)')
    parser.add_argument('--log-file', nargs='?', const=default_log_path(),
                        help='also write logging to a file '
                             f'(default: {default_log_path()})')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='include debug logging')
    args = parser.parse_args(argv)

    configure_logging(args.verbose, args.log_file)

    if args.list_ports:
        found = candidate_ports()
        if not found:
            print('No SmartKnob-like serial ports found.')
        for port in found:
            print(f'{port.device} - {port.description}')
        return 0

    if args.stop or args.restart:
        stopped = instance.stop_running()
        print(f'Stopped agent {", ".join(str(p) for p in stopped)}.' if stopped
              else 'No agent was running.')
        if not args.restart:
            print('The serial port is free now.')
            return 0
        if instance.start_detached():
            print('A fresh hidden agent is running.')
            return 0
        return 1

    keys = [key.strip() for key in args.controls.split(',') if key.strip()]
    if args.self_test:
        return _self_test(keys)

    if not args.allow_multiple and not instance.acquire_single_instance():
        # Two agents would take turns kicking each other off the port, which
        # looks like a flaky knob rather than the duplicate launch it is.
        logger.warning('Another SmartKnob agent is already running; leaving the '
                       'port to it. Use --restart to replace it.')
        return 0

    try:
        controls = build_controls(keys)
    except ControlUnavailable as e:
        parser.error(str(e))

    logger.info('Serving: %s', ', '.join(c.label for c in controls))

    while True:
        try:
            port = args.port or find_port()
            if port is None:
                # Nothing plugged in yet. Waiting is the whole point of running
                # in the background, so this isn't an error.
                logger.debug('No SmartKnob found; waiting')
                reconnect = True
            else:
                reconnect = run_once(port, controls, args.poll_interval)
        except KeyboardInterrupt:
            reconnect = False
        except (serial.SerialException, OSError) as e:
            logger.warning('Serial error: %s', e)
            reconnect = True

        if not reconnect or args.no_reconnect:
            logger.info('Bye')
            return 0
        time.sleep(RECONNECT_DELAY_SECONDS)


if __name__ == '__main__':
    sys.exit(main())

