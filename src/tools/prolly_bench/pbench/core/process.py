"""Bounded subprocesses; terminate the whole client group on interruption."""
import os
import signal
import subprocess
import hashlib
import tempfile
import threading


def run(args, *, timeout=7200, capture_output=False, input=None, check=False, **kwargs):
    if capture_output:
        kwargs.update(stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if input is not None:
        kwargs["stdin"] = subprocess.PIPE
    with subprocess.Popen(args, start_new_session=True, **kwargs) as proc:
        try:
            stdout, stderr = proc.communicate(input, timeout=timeout)
        except BaseException:
            # JVMs and shells can leave grandchildren alive after killing only
            # the parent. Never let a timed-out cell contaminate the next one.
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.communicate()
            raise
        result = subprocess.CompletedProcess(args, proc.returncode, stdout, stderr)
        if check:
            result.check_returncode()
        return result


def hash_output(args, *, env=None, timeout=7200):
    """Hash arbitrarily large output without keeping it in RAM or on disk."""
    with tempfile.TemporaryFile() as errors, subprocess.Popen(
            args, stdout=subprocess.PIPE, stderr=errors, env=env, start_new_session=True) as proc:
        expired = threading.Event()
        def stop():
            expired.set()
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        timer = threading.Timer(timeout, stop)
        timer.daemon = True
        timer.start()
        try:
            h = hashlib.sha256()
            for block in iter(lambda: proc.stdout.read(1024 * 1024), b""):
                h.update(block)
            code = proc.wait()
            if expired.is_set():
                raise subprocess.TimeoutExpired(args, timeout)
            if code:
                errors.seek(0)
                raise RuntimeError(f"streamed command failed ({code}): {errors.read(2000).decode(errors='replace')}")
            return h.hexdigest()
        finally:
            timer.cancel()
            if proc.poll() is None:
                stop()
                proc.wait()
