"""Create a deterministic Picker MAX archive and its release file pins."""
from argparse import ArgumentParser
from hashlib import sha256
import json
from pathlib import Path
from zipfile import ZipFile, ZipInfo

FILES = (
    'ascent_picker_overlay',
    'ascent_picker_rt',
    'libascent_frame_overlay.so',
    'libascent_uart_mirror_tap.so',
    'picker-start.sh',
)


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument('build_directory', type=Path)
    parser.add_argument('archive', type=Path)
    args = parser.parse_args()
    blobs = {name: (args.build_directory / name).read_bytes() for name in FILES}
    with ZipFile(args.archive, 'x') as package:
        for name, data in blobs.items():
            entry = ZipInfo(name)
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            package.writestr(entry, data)
    print(json.dumps({
        'sha256': sha256(args.archive.read_bytes()).hexdigest(),
        'files': {name: sha256(data).hexdigest() for name, data in blobs.items()},
    }, indent=2))


if __name__ == '__main__':
    main()
