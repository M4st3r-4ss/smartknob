"""
Tests for the SmartKnob deck logic.

These only exercise the host-side logic, so they need no hardware and no
third-party packages:
    python -m unittest test_smartknob_deck
"""

import logging
import math
import unittest

from smartknob_deck.controller import (
    IDLE_BEFORE_POLL_SECONDS,
    DeckAgent,
)
from smartknob_deck.controls import (
    ALL_CONTROLS,
    Control,
    ControlUnavailable,
    build_controls,
)


class FakeClock(object):
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


class FullRangeControl(Control):
    """Bare Control, for checking the travel maths the real controls inherit."""


class FakeControl(Control):
    # Pinned so agent tests don't depend on the travel budget
    position_width_radians = math.radians(5)

    def __init__(self, key='fake', label='Fake', value=0, min_value=0,
                 max_value=100, led_hue=10, min_write_interval=0):
        self.key = key
        self.label = label
        self.min_value = min_value
        self.max_value = max_value
        self.led_hue = led_hue
        self.min_write_interval = min_write_interval
        self.value = value
        self.writes = []
        self.read_error = None
        self.write_error = None

    def _read(self):
        if self.read_error is not None:
            raise self.read_error
        return self.value

    def _write(self, value):
        if self.write_error is not None:
            raise self.write_error
        self.value = value
        self.writes.append(value)


def make_agent(controls, clock=None, poll_interval=2.0):
    """Build an agent whose outbound lines land in the returned list."""
    sent = []
    agent = DeckAgent(
        controls,
        lambda verb, channel, value=None: sent.append((verb, channel, value)),
        poll_interval=poll_interval,
        clock=clock or FakeClock(),
    )
    return agent, sent


class ControlMappingTest(unittest.TestCase):
    def test_position_is_the_value(self):
        control = FakeControl(min_value=5, max_value=100)
        self.assertEqual(control.value_for_position(5), 5)
        self.assertEqual(control.value_for_position(37), 37)
        self.assertEqual(control.position_for_value(100), 100)

    def test_values_are_clamped_to_the_range(self):
        control = FakeControl(min_value=5, max_value=100)
        self.assertEqual(control.value_for_position(-3), 5)
        self.assertEqual(control.value_for_position(140), 100)
        self.assertEqual(control.position_for_value(-10), 5)
        self.assertEqual(control.position_for_value(500), 100)

    def test_read_returns_an_int_in_range(self):
        control = FakeControl(min_value=5, max_value=100, value=42.6)
        self.assertEqual(control.read(), 43)

    def test_write_clamps_before_touching_the_os(self):
        control = FakeControl(min_value=0, max_value=100)
        control.write(240)
        control.write(-8)
        self.assertEqual(control.writes, [100, 0])

    def test_build_controls_rejects_unknown_keys(self):
        with self.assertRaises(ControlUnavailable):
            build_controls(['nope'])


class TravelTest(unittest.TestCase):
    """Every control sweeps the same 300 degrees, centred by the firmware."""

    def travel_degrees(self, control):
        span = control.max_value - control.min_value
        return span * math.degrees(control.position_width_radians)

    def test_every_real_control_travels_300_degrees(self):
        for key, factory in sorted(ALL_CONTROLS.items()):
            with self.subTest(control=key):
                # Constructed directly: no OS access happens until read/write
                self.assertAlmostEqual(self.travel_degrees(factory()), 300,
                                       places=6)

    def test_detent_width_follows_the_range(self):
        # 100 steps over 300 degrees is 3 degrees each...
        self.assertAlmostEqual(
            math.degrees(FullRangeControl().position_width_radians), 3.0)
        # ...and a narrower range spreads the same travel over fewer steps
        narrow = FullRangeControl()
        narrow.min_value, narrow.max_value = 0, 10
        self.assertAlmostEqual(
            math.degrees(narrow.position_width_radians), 30.0)
        self.assertAlmostEqual(self.travel_degrees(narrow), 300)

    def test_single_value_range_does_not_divide_by_zero(self):
        control = FullRangeControl()
        control.min_value = control.max_value = 50
        self.assertAlmostEqual(
            math.degrees(control.position_width_radians), 300)


class StartupTest(unittest.TestCase):
    def test_start_reports_every_channel(self):
        volume = FakeControl(key='volume', label='Volume', value=30)
        bright = FakeControl(key='brightness', label='Brightness', value=70)
        agent, sent = make_agent([volume, bright])

        agent.start()

        self.assertEqual(sent, [('VAL', 'brightness', 70),
                                ('VAL', 'volume', 30)])

    def test_unreadable_channel_reports_nothing(self):
        control = FakeControl(key='volume')
        control.read_error = ControlUnavailable('no audio endpoint')
        agent, sent = make_agent([control])

        with self.assertLogs('smartknob_deck', level='WARNING'):
            agent.start()

        # Better to leave the knob on its own value than to lie about the OS.
        self.assertEqual(sent, [])
        self.assertIsNone(agent.value('volume'))

    def test_empty_control_list_is_rejected(self):
        with self.assertRaises(ValueError):
            make_agent([])


class SetTest(unittest.TestCase):
    def test_set_writes_the_value(self):
        control = FakeControl(key='volume')
        agent, _ = make_agent([control])

        agent.handle_line('@SET volume 42')

        self.assertEqual(control.writes, [42])
        self.assertEqual(agent.value('volume'), 42)

    def test_set_is_clamped_to_the_control_range(self):
        control = FakeControl(key='volume', min_value=5, max_value=90)
        agent, _ = make_agent([control])

        agent.handle_line('@SET volume 300')
        agent.handle_line('@SET volume -20')

        self.assertEqual(control.writes, [90, 5])

    def test_writes_are_coalesced_while_rate_limited(self):
        clock = FakeClock()
        control = FakeControl(key='brightness', min_write_interval=0.15)
        agent, _ = make_agent([control], clock=clock)

        for value in (10, 11, 12, 13):
            agent.handle_line(f'@SET brightness {value}')

        # Only the first got through; the rest collapsed into one pending write.
        self.assertEqual(control.writes, [10])
        clock.advance(0.2)
        agent.tick()
        self.assertEqual(control.writes, [10, 13])

    def test_failed_write_does_not_stick_around(self):
        control = FakeControl(key='volume')
        control.write_error = ControlUnavailable('device disappeared')
        agent, _ = make_agent([control])

        with self.assertLogs('smartknob_deck', level='WARNING'):
            agent.handle_line('@SET volume 55')
        agent.tick()

        self.assertEqual(control.writes, [])

    def test_unknown_channel_is_ignored(self):
        control = FakeControl(key='volume')
        agent, sent = make_agent([control])

        agent.handle_line('@SET fan 3')
        agent.handle_line('@GET fan')

        self.assertEqual(control.writes, [])
        self.assertEqual(sent, [])

    def test_malformed_lines_are_ignored(self):
        control = FakeControl(key='volume')
        agent, sent = make_agent([control])

        for line in ('', 'hello', '@SET', '@SET volume', '@SET volume loud',
                     'SET volume 40', '@'):
            agent.handle_line(line)

        self.assertEqual(control.writes, [])
        self.assertEqual(sent, [])


class GetTest(unittest.TestCase):
    def test_get_answers_with_the_os_value(self):
        control = FakeControl(key='volume', value=64)
        agent, sent = make_agent([control])

        agent.handle_line('@GET volume')

        self.assertEqual(sent, [('VAL', 'volume', 64)])

    def test_get_reflects_a_later_external_change(self):
        control = FakeControl(key='volume', value=20)
        agent, sent = make_agent([control])

        agent.handle_line('@GET volume')
        control.value = 80
        agent.handle_line('@GET volume')

        self.assertEqual(sent, [('VAL', 'volume', 20), ('VAL', 'volume', 80)])


class ExternalChangeTest(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.control = FakeControl(key='volume', value=30)
        # Polling faster than the quiet window, so both gates are visible here.
        self.agent, self.sent = make_agent([self.control], clock=self.clock,
                                           poll_interval=0.1)
        self.agent.start()
        self.sent.clear()

    def settle(self):
        """Move past both the poll interval and the post-rotation quiet time."""
        self.clock.advance(IDLE_BEFORE_POLL_SECONDS + 0.1)

    def test_external_change_is_pushed_to_the_knob(self):
        self.control.value = 75
        self.settle()
        self.agent.tick()

        self.assertEqual(self.sent, [('VAL', 'volume', 75)])
        self.assertEqual(self.agent.value('volume'), 75)

    def test_unchanged_value_is_not_resent(self):
        self.settle()
        self.agent.tick()
        self.settle()
        self.agent.tick()

        self.assertEqual(self.sent, [])

    def test_polling_waits_until_the_knob_settles(self):
        self.agent.handle_line('@SET volume 40')
        self.control.value = 90  # something else moved it mid-turn

        self.clock.advance(0.2)
        self.agent.tick()
        # Still inside the quiet window, so the knob keeps control.
        self.assertEqual(self.sent, [])

        self.settle()
        self.agent.tick()
        self.assertEqual(self.sent, [('VAL', 'volume', 90)])

    def test_pending_write_defers_polling(self):
        clock = FakeClock()
        control = FakeControl(key='brightness', value=30, min_write_interval=10)
        agent, sent = make_agent([control], clock=clock, poll_interval=0.5)

        agent.handle_line('@SET brightness 40')  # written immediately
        agent.handle_line('@SET brightness 45')  # queued behind the interval
        clock.advance(5)
        agent.tick()

        # Reading now would return the stale 40 and fight the queued write.
        self.assertEqual(sent, [])

    def test_unreadable_control_does_not_break_polling(self):
        self.control.read_error = ControlUnavailable('device disappeared')
        self.settle()
        self.agent.tick()

        self.assertEqual(self.sent, [])
        self.assertEqual(self.agent.value('volume'), 30)

    def test_channels_are_polled_independently(self):
        clock = FakeClock()
        volume = FakeControl(key='volume', value=10)
        bright = FakeControl(key='brightness', value=20)
        agent, sent = make_agent([volume, bright], clock=clock,
                                 poll_interval=0.1)
        agent.start()
        sent.clear()

        agent.handle_line('@SET volume 15')
        bright.value = 60
        clock.advance(IDLE_BEFORE_POLL_SECONDS - 0.1)
        agent.tick()

        # Brightness is free to report even though volume is still settling.
        self.assertEqual(sent, [('VAL', 'brightness', 60)])


def setUpModule():
    # Keep the agent's INFO chatter out of the test output; assertLogs still
    # captures what the tests care about.
    logging.getLogger('smartknob_deck').setLevel(logging.WARNING)


if __name__ == '__main__':
    unittest.main()
