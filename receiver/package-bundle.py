"""Build a Picker host and receiver qualification bundle from reviewed inputs."""
from argparse import ArgumentParser
from hashlib import sha256
import json
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parents[1]
VARIANTS = {
    'classic': ('1.2.0', '32642d8c19196852de445b5277fed3f23f7dc5dd0deaa9fd895f458b20aeaa2b'),
    'max': ('1.2.1', 'c6ec4c07a67c185fa6cdca79b2ace5297d8a48a342820c0b0603c8ce397c25a2'),
}


def build(classic, max_variant, output):
    profiles = {}
    payload = {}
    for profile, path in (('classic', classic), ('max', max_variant)):
        version, expected = VARIANTS[profile]
        data = path.read_bytes()
        digest = sha256(data).hexdigest()
        if digest != expected:
            raise ValueError(f'Unexpected Picker {profile} variant digest')
        name = f'variants/{profile}.zip'
        profiles[profile] = {'version': version, 'file': name, 'sha256': digest}
        payload[name] = data
    payload['host/caddx_picker_cells.py'] = (ROOT / 'cells_projection.py').read_bytes()
    projection = (ROOT / 'picker_projection.py').read_bytes()
    source_import = b'import cells_projection as pc'
    if projection.count(source_import) != 1:
        raise ValueError('The Picker host import needs review')
    payload['host/caddx_picker_projection.py'] = projection.replace(
        source_import, b'import caddx_picker_cells as pc', 1)
    manifest = {'schema': 'ascent.receiver_widget_bundle.v3', 'id': 'picker',
                'version': VARIANTS['max'][0], 'profiles': profiles}
    payload['bundle.json'] = (json.dumps(manifest, sort_keys=True) + '\n').encode()
    with ZipFile(output, 'x') as archive:
        for name, data in sorted(payload.items()):
            archive.writestr(ZipInfo(name), data, compress_type=ZIP_DEFLATED)
    return sha256(output.read_bytes()).hexdigest()


if __name__ == '__main__':
    parser = ArgumentParser(description=__doc__)
    parser.add_argument('classic_zip', type=Path)
    parser.add_argument('max_zip', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(build(args.classic_zip, args.max_zip, args.output))
