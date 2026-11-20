"""Package a verified Classic Gnd485 Picker owner for native lifecycle integration."""

from argparse import ArgumentParser
from hashlib import sha256
import json
from pathlib import Path
from zipfile import ZipFile, ZipInfo

OWNER = 'ascent_picker_rt'
PROFILE = 'native-gnd485-19.2.14'
BENCH_OWNER_SHA256 = '52a7c762d95dc13da22810e6c2b2c745a9d4dc7df02de8d77f38de11541a1ced'


def package(build_directory, archive):
    build = json.loads((build_directory / 'build.json').read_bytes())
    owner = (build_directory / OWNER).read_bytes()
    digest = sha256(owner).hexdigest()
    if (build.get('profile') != PROFILE or build.get('classic_release') is not True
            or build.get('sha256') != digest or digest != BENCH_OWNER_SHA256):
        raise ValueError('Expected the reviewed Classic Picker owner build')
    with ZipFile(archive, 'x') as output:
        entry = ZipInfo(OWNER)
        entry.create_system = 3
        entry.external_attr = 0o100755 << 16
        output.writestr(entry, owner)
    return {
        'profile': PROFILE,
        'requires_native_payload_revision': 16,
        'sha256': sha256(archive.read_bytes()).hexdigest(),
        'files': {OWNER: digest},
    }


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument('build_directory', type=Path)
    parser.add_argument('archive', type=Path)
    args = parser.parse_args()
    print(json.dumps(package(args.build_directory, args.archive), indent=2))


if __name__ == '__main__':
    main()
