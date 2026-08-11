"""
Keep one agent running, and let a firmware upload borrow the serial port.

The problem that prompted this: while the agent holds the port, esptool cannot
flash the knob. It dies with "could not open port 'COM6': Access is denied", and
nothing in the message hints at what is holding it. So the running agent has to
be stoppable and restartable - by --stop/--restart, and automatically by the
PlatformIO hook in scripts/deck_upload_hook.py.

The single-instance lock is belt and braces for the other way to lose the port:
two agents would take turns kicking each other off it, which would read as a
flaky knob rather than the duplicate launch it is. install_autostart run twice,
or run while a login copy is up, is the easy way to get there.
"""

import logging
import os
import subprocess
import sys
import tempfile

logger = logging.getLogger('smartknob_deck')

# "Local\" scopes the mutex to this login session, which is the scope we want:
# another user's agent talks to their own knob on their own port.
MUTEX_NAME = 'Local\\SmartKnobDeckAgent'
ERROR_ALREADY_EXISTS = 183

# Anything holding these stays alive for the life of the process; Windows drops
# the mutex and the OS drops the file lock when we exit, however we exit.
_held = []


def acquire_single_instance():
    """True if we are the only agent, False if another one already holds the lock."""
    if os.name != 'nt':
        return _acquire_posix()

    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.CreateMutexW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR]
    kernel32.CreateMutexW.restype = wintypes.HANDLE

    handle = kernel32.CreateMutexW(None, False, MUTEX_NAME)
    if not handle:
        # We could not even ask. Refusing to run over a lock we cannot take
        # would be worse than the double-copy we are trying to avoid.
        logger.debug('CreateMutexW failed (%s); skipping the guard',
                     ctypes.get_last_error())
        return True
    if ctypes.get_last_error() == ERROR_ALREADY_EXISTS:
        kernel32.CloseHandle(handle)
        return False
    _held.append(handle)
    return True


def _acquire_posix():
    try:
        import fcntl
    except ImportError:
        return True
    path = os.path.join(tempfile.gettempdir(), 'smartknob_deck.lock')
    f = open(path, 'w')
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        f.close()
        return False
    _held.append(f)
    return True


def running_pids():
    """PIDs of other agent processes, newest first."""
    pids = _all_agent_pids()
    return [pid for pid in pids if pid != os.getpid()]


def _all_agent_pids():
    if os.name != 'nt':
        return _posix_agent_pids()
    # Match on the interpreter name as well as the command line: a shell or a
    # PowerShell command that merely mentions smartknob_deck is not an agent,
    # and killing the thing that asked would be a memorable bug.
    #
    # Expect ONE agent to show up as TWO pids with identical command lines: the
    # venv's pythonw.exe is a shim that spawns the real interpreter as its child.
    # Both have to be stopped - killing only the shim orphans the child that is
    # actually holding the serial port.
    script = (
        "Get-CimInstance Win32_Process | "
        "Where-Object { $_.CommandLine -like '*smartknob_deck*' "
        "-and $_.Name -like 'python*' } | "
        "ForEach-Object { $_.ProcessId }"
    )
    try:
        done = subprocess.run(
            ['powershell', '-NoProfile', '-NonInteractive', '-Command', script],
            capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError) as e:
        logger.debug('Could not list processes: %s', e)
        return []
    pids = []
    for token in done.stdout.split():
        try:
            pids.append(int(token))
        except ValueError:
            pass
    return pids


def _posix_agent_pids():
    try:
        done = subprocess.run(['ps', '-eo', 'pid=,args='],
                              capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return []
    pids = []
    for line in done.stdout.splitlines():
        pid, _, args = line.strip().partition(' ')
        if 'smartknob_deck' in args and 'python' in args:
            try:
                pids.append(int(pid))
            except ValueError:
                pass
    return pids


def stop_running(timeout=6.0):
    """
    Stop every other agent and wait for the port to actually be free.

    Returns the PIDs that were asked to stop. Waiting matters: esptool opens the
    port the moment we return, and a process that is merely 'signalled' still
    holds its handle.
    """
    pids = running_pids()
    for pid in pids:
        _terminate(pid)
    if pids:
        _wait_until_gone(timeout)
    return pids


def _terminate(pid):
    try:
        if os.name == 'nt':
            # The agent runs under pythonw with no console, so there is no
            # CTRL_BREAK to send. It keeps nothing buffered worth saving.
            subprocess.run(['taskkill', '/F', '/PID', str(pid)],
                           capture_output=True, timeout=15)
        else:
            import signal
            os.kill(pid, signal.SIGTERM)
    except (OSError, subprocess.SubprocessError) as e:
        logger.debug('Could not stop pid %s: %s', pid, e)


def _wait_until_gone(timeout):
    import time
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not running_pids():
            return True
        time.sleep(0.25)
    return not running_pids()


def launcher_path():
    """The hidden .vbs launcher install_autostart wrote, if it is there."""
    from .install_autostart import VBS_NAME, app_dir
    path = os.path.join(app_dir(), VBS_NAME)
    return path if os.path.exists(path) else None


def start_detached(extra_args=None):
    """
    Launch a hidden agent that outlives this process.

    Prefers the .vbs launcher, so a restart looks exactly like a login start
    (same arguments, same working directory, no console flash).
    """
    if running_pids():
        return True

    vbs = launcher_path()
    try:
        if vbs is not None and not extra_args:
            subprocess.Popen(['wscript', vbs], close_fds=True)
            return True
        return _start_directly(extra_args or ['--log-file'])
    except (OSError, subprocess.SubprocessError) as e:
        logger.warning('Could not start the agent: %s', e)
        return False


def _start_directly(extra_args):
    from .install_autostart import package_root, pythonw_path

    kwargs = {'cwd': package_root(), 'close_fds': True}
    if os.name == 'nt':
        # DETACHED_PROCESS so it survives us; CREATE_NO_WINDOW so nothing flashes.
        kwargs['creationflags'] = 0x00000008 | 0x08000000
    else:
        kwargs['start_new_session'] = True

    interpreter = pythonw_path() if os.name == 'nt' else sys.executable
    subprocess.Popen([interpreter, '-m', 'smartknob_deck'] + list(extra_args),
                     **kwargs)
    return True
