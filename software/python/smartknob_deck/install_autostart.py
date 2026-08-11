"""
Set up the deck agent to run automatically, with no console window and no
development environment:

    py -m smartknob_deck.install_autostart

That drops two files in the user's Startup folder region:

  * a .vbs shim, which is the only way to launch a process on Windows with no
    console flash at all
  * a shortcut to it in shell:startup, so it starts at login

Undo with --uninstall. Nothing outside the current user's profile is touched, so
this needs no administrator rights.
"""

import argparse
import os
import subprocess
import sys

APP_NAME = 'SmartKnob Deck'
VBS_NAME = 'smartknob_deck_launch.vbs'
SHORTCUT_NAME = APP_NAME + '.lnk'


def app_dir():
    base = os.environ.get('LOCALAPPDATA') or os.path.expanduser('~')
    return os.path.join(base, 'SmartKnob')


def startup_dir():
    return os.path.join(os.environ['APPDATA'], 'Microsoft', 'Windows',
                        'Start Menu', 'Programs', 'Startup')


def pythonw_path():
    """The console-less interpreter next to the one running this."""
    candidate = os.path.join(os.path.dirname(sys.executable), 'pythonw.exe')
    return candidate if os.path.exists(candidate) else sys.executable


def package_root():
    """The directory holding the smartknob_deck package."""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def write_vbs(path, args):
    interpreter = pythonw_path()
    command = ' '.join(_quote(part) for part in
                       [interpreter, '-m', 'smartknob_deck'] + args)
    # 0 = hidden window, False = don't wait for it to exit.
    script = (
        f'Set shell = CreateObject("WScript.Shell")\n'
        f'shell.CurrentDirectory = {_vbs_string(package_root())}\n'
        f'shell.Run {_vbs_string(command)}, 0, False\n'
    )
    with open(path, 'w', encoding='ascii') as f:
        f.write(script)


def _quote(part):
    return f'"{part}"' if ' ' in part else part


def _vbs_string(value):
    return '"' + value.replace('"', '""') + '"'


def make_shortcut(shortcut_path, target):
    """Create the Startup shortcut via the Windows shell, using PowerShell."""
    script = (
        '$s = (New-Object -ComObject WScript.Shell).CreateShortcut('
        f'{_ps_string(shortcut_path)});'
        f'$s.TargetPath = {_ps_string(target)};'
        f'$s.WorkingDirectory = {_ps_string(os.path.dirname(target))};'
        f'$s.Description = {_ps_string(APP_NAME)};'
        '$s.Save()'
    )
    subprocess.run(
        ['powershell', '-NoProfile', '-NonInteractive', '-Command', script],
        check=True, capture_output=True)


def _ps_string(value):
    return "'" + value.replace("'", "''") + "'"


def install(extra_args):
    os.makedirs(app_dir(), exist_ok=True)
    vbs_path = os.path.join(app_dir(), VBS_NAME)
    write_vbs(vbs_path, extra_args)

    os.makedirs(startup_dir(), exist_ok=True)
    shortcut_path = os.path.join(startup_dir(), SHORTCUT_NAME)
    make_shortcut(shortcut_path, vbs_path)

    print(f'Launcher:  {vbs_path}')
    print(f'Startup:   {shortcut_path}')
    print()
    # Installing twice would otherwise leave two agents taking turns kicking each
    # other off the serial port, which reads as a flaky knob. Replace, don't add.
    from . import instance
    stopped = instance.stop_running()
    if stopped:
        print(f'Replacing the agent already running '
              f'({", ".join(str(pid) for pid in stopped)}).')
    print('Starting it now so you don\'t have to log out and back in...')
    subprocess.Popen(['wscript', vbs_path], close_fds=True)
    print('Running. Turn the knob and the volume should follow.')
    print()
    print('Log: ' + os.path.join(app_dir(), 'smartknob_deck.log'))
    print('Stop it with: py -m smartknob_deck --stop')
    print('Remove it with: py -m smartknob_deck.install_autostart --uninstall')
    return 0


def uninstall():
    removed = []
    for path in (os.path.join(startup_dir(), SHORTCUT_NAME),
                 os.path.join(app_dir(), VBS_NAME)):
        try:
            os.remove(path)
            removed.append(path)
        except FileNotFoundError:
            pass
    for path in removed:
        print(f'Removed {path}')
    if not removed:
        print('Nothing to remove.')

    # Uninstalling and then finding the knob still driving the volume until the
    # next reboot would be baffling, so stop the running copy as well.
    from . import instance
    stopped = instance.stop_running()
    if stopped:
        print(f'Stopped the running agent '
              f'({", ".join(str(pid) for pid in stopped)}).')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog='python -m smartknob_deck.install_autostart',
        description='Run the SmartKnob deck agent at login, hidden.')
    parser.add_argument('--uninstall', action='store_true',
                        help='remove the launcher and the Startup entry')
    parser.add_argument('--port', help='pin a serial port instead of '
                                       'auto-detecting the knob')
    parser.add_argument('--controls', help='comma-separated controls to serve')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='run the agent with debug logging')
    args = parser.parse_args(argv)

    if os.name != 'nt':
        parser.error('this installer is Windows-only')

    if args.uninstall:
        return uninstall()

    extra = ['--log-file']
    if args.port:
        extra += ['--port', args.port]
    if args.controls:
        extra += ['--controls', args.controls]
    if args.verbose:
        extra.append('--verbose')
    return install(extra)


if __name__ == '__main__':
    sys.exit(main())
