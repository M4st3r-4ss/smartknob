"""
PlatformIO hook: hand the serial port to esptool for an upload, then give it back.

The deck agent (software/python/smartknob_deck) holds the knob's USB serial port
open all the time, which is the whole point of it - but esptool cannot share it.
Without this hook every flash fails with:

    could not open port 'COM6': PermissionError(13, 'Access is denied')

So: stop the agent before uploading, start it again afterwards. If no agent is
running, or this is not the machine the agent lives on, the hook does nothing.

Wired up from platformio.ini:

    extra_scripts = post:scripts/deck_upload_hook.py

Nothing here is allowed to fail an upload. A broken hook must not stand between
the user and their firmware, so all the work happens inside guarded callbacks -
module level does nothing that can raise. (PlatformIO exec's this file, so there
is no __file__ here either; the project directory comes from the SCons env.)
"""

import os
import sys

Import('env')  # noqa: F821 - injected by PlatformIO/SCons

# Set when we stopped an agent, so we only restart what we actually took down.
_stopped = []


def _load_instance(env):
    """
    Import the agent's process control into PlatformIO's own interpreter.

    smartknob_deck.instance is stdlib-only by design, so this works without
    pyserial or the Windows control backends being installed here.
    """
    package_dir = os.path.join(env.subst('$PROJECT_DIR'), 'software', 'python')
    if package_dir not in sys.path:
        sys.path.insert(0, package_dir)
    from smartknob_deck import instance
    return instance


def before_upload(source, target, env):
    global _stopped
    _stopped = []
    try:
        _stopped = _load_instance(env).stop_running()
    except Exception as e:  # noqa: BLE001 - never block an upload
        print(f'deck_upload_hook: could not check for a running agent ({e})')
        return
    if _stopped:
        pids = ', '.join(str(pid) for pid in _stopped)
        print(f'deck_upload_hook: stopped the SmartKnob deck agent ({pids}) '
              'to free the serial port')


def after_upload(source, target, env):
    if not _stopped:
        return
    try:
        started = _load_instance(env).start_detached()
    except Exception as e:  # noqa: BLE001
        print(f'deck_upload_hook: could not restart the agent ({e})')
        print('deck_upload_hook: start it yourself with '
              '"py -m smartknob_deck --restart"')
        return
    print('deck_upload_hook: deck agent running again' if started else
          'deck_upload_hook: could not restart the agent; run '
          '"py -m smartknob_deck --restart"')


env.AddPreAction('upload', before_upload)   # noqa: F821
env.AddPostAction('upload', after_upload)   # noqa: F821
