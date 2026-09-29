"""Shared compiler invocation for the hardware-free C tests."""
from pathlib import Path
import subprocess
import tempfile


def compile_and_run(source, *, cc='cc', flags=(), sources=()):
    with tempfile.TemporaryDirectory(prefix='apex-host-test-') as folder:
        path = Path(folder)
        (path / 'test.c').write_text(source)
        binary = path / 'test.exe'
        subprocess.run([cc, '-std=c99', '-Wall', '-Wextra', '-Werror', *flags,
                        str(path / 'test.c'), *(str(p) for p in sources),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
