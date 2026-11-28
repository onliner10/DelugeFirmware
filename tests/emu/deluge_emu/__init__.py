"""Run the Deluge firmware in the delugemu emulator, with crash detection.

See tests/emu/README.md. Entry points:

* ``./dbt emu ...``           - command line (setup, build, test, run, start/ctl/stop)
* ``EmuSession.start(...)``   - Python API (tests, ad-hoc scripts)
"""


def __getattr__(name):
    # Lazy: the session module needs delugemu's pylib, which only exists after
    # `./dbt emu setup`; `setup` itself must be importable before that.
    if name in ("EmuSession", "EmuCrash"):
        from . import session

        return getattr(session, name)
    raise AttributeError(name)
