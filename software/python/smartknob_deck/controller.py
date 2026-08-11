"""
Deck logic: applies values the knob sends, and reports OS-side changes back.

The knob owns its own menu, haptics and display; this agent is a follower. It
speaks the plaintext host-agent lines that the firmware emits:

    @SET <channel> <value>   knob was turned -> apply it to the OS
    @GET <channel>           knob opened a page -> answer with the OS value

and replies with:

    @VAL <channel> <value>   the OS-side value of a channel

It also watches for changes made elsewhere (volume keys, another app) and pushes
those back as @VAL so the knob's position stays truthful.

This module is deliberately free of serial and OS dependencies so it can be unit
tested on its own. The caller supplies a `sender` callable that puts one line on
the wire, and feeds inbound lines into handle_line().
"""

import logging
import time

from .controls import ControlUnavailable

logger = logging.getLogger(__name__)

# How long after the knob last moved a channel before we trust the OS's value
# for it again. Without this, a slow volume/brightness write could be read back
# mid-turn and fight the user for control of the position.
IDLE_BEFORE_POLL_SECONDS = 0.75


class DeckAgent(object):
    """Applies knob values to OS controls and answers the knob's queries."""

    def __init__(self, controls, sender, poll_interval=2.0, clock=time.monotonic):
        if not controls:
            raise ValueError('need at least one control')
        self._controls = {control.key: control for control in controls}
        self._sender = sender
        self._poll_interval = poll_interval
        self._clock = clock

        # Last value we believe each channel is at, whichever side set it.
        self._values = {}
        # Coalesced writes: at most one OS write per channel per interval.
        self._pending = {}
        self._last_write = {}
        self._last_knob_move = {}
        self._last_poll = 0.0

    @property
    def channels(self):
        return sorted(self._controls)

    def value(self, channel):
        """Value the agent believes a channel is currently at, or None."""
        return self._values.get(channel)

    def start(self):
        """Report every channel's current value, so the knob starts in sync."""
        for channel in self.channels:
            self._report(channel)

    def handle_line(self, line):
        """Process one host-agent line from the knob. Unknown lines are ignored."""
        line = line.strip()
        if not line.startswith('@'):
            return
        parts = line[1:].split()
        if len(parts) < 2:
            return
        verb, channel = parts[0].upper(), parts[1]

        if channel not in self._controls:
            logger.debug('Ignoring %s for unknown channel "%s"', verb, channel)
            return

        if verb == 'GET':
            self._report(channel)
        elif verb == 'SET' and len(parts) >= 3:
            try:
                value = int(parts[2])
            except ValueError:
                logger.debug('Ignoring unparseable value in "%s"', line)
                return
            self._apply(channel, value)

    def tick(self):
        """Periodic work: flush coalesced writes, follow external changes."""
        self._flush_pending()
        self._poll_external()

    def _apply(self, channel, value):
        control = self._controls[channel]
        value = control.clamp(value)
        self._values[channel] = value
        self._pending[channel] = value
        self._last_knob_move[channel] = self._clock()
        self._flush_pending()

    def _flush_pending(self):
        now = self._clock()
        for channel in list(self._pending):
            control = self._controls[channel]
            if now - self._last_write.get(channel, 0.0) < control.min_write_interval:
                continue
            value = self._pending.pop(channel)
            self._last_write[channel] = now
            try:
                control.write(value)
            except ControlUnavailable as e:
                logger.warning('Could not set %s: %s', control.label, e)
            else:
                logger.debug('%s -> %d', control.label, value)

    def _poll_external(self):
        """Follow changes made elsewhere and push them to the knob."""
        now = self._clock()
        if now - self._last_poll < self._poll_interval:
            return
        self._last_poll = now

        for channel in self.channels:
            if channel in self._pending:
                continue
            if now - self._last_knob_move.get(channel, 0.0) < IDLE_BEFORE_POLL_SECONDS:
                continue
            control = self._controls[channel]
            try:
                value = control.read()
            except ControlUnavailable as e:
                logger.debug('Could not read %s: %s', control.label, e)
                continue
            if value != self._values.get(channel):
                logger.info('%s changed externally: %s -> %d', control.label,
                            self._values.get(channel), value)
                self._values[channel] = value
                self._sender('VAL', channel, value)

    def _report(self, channel):
        """Read a channel and send its value to the knob."""
        control = self._controls[channel]
        try:
            value = control.read()
        except ControlUnavailable as e:
            logger.warning('Could not read %s: %s', control.label, e)
            # Nothing truthful to send; leave the knob on its own value.
            return
        self._values[channel] = value
        self._last_poll = self._clock()
        logger.info('%s: %d', control.label, value)
        self._sender('VAL', channel, value)
