import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
files = sorted(str(path) for name in ('core', 'boot', 'platform', 'app', 'cli', 'ui', 'tests')
               for path in (root / name).rglob('*') if path.suffix in ('.cpp', '.hpp'))
raise SystemExit(subprocess.call([sys.argv[1] if len(sys.argv) > 1 else 'clang-format', '--dry-run', '--Werror', *files]))
