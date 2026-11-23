"""Build a Picker host and receiver qualification bundle from reviewed inputs."""
from argparse import ArgumentParser
from hashlib import sha256
from io import BytesIO
import json
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parents[1]
VARIANTS = {
    'classic': ('1.2.0', '32642d8c19196852de445b5277fed3f23f7dc5dd0deaa9fd895f458b20aeaa2b'),
    'max': ('1.2.1', 'c6ec4c07a67c185fa6cdca79b2ace5297d8a48a342820c0b0603c8ce397c25a2'),
}


CLASSIC_OWNER = 'ascent_picker_rt'
CLASSIC_OWNER_ARCHIVE_SHA256 = '18a7edd2b58eacc75150967b5908dc2fa3e9bcfc4ecf62aa524b7c4c33e46c21'


def upgrade_classic_variant(variant, owner_archive):
    if sha256(owner_archive).hexdigest() != CLASSIC_OWNER_ARCHIVE_SHA256:
        raise ValueError('Unexpected Classic Picker revision 16 owner archive digest')
    with ZipFile(BytesIO(owner_archive)) as owner, ZipFile(BytesIO(variant)) as base:
        files = {name: base.read(name) for name in base.namelist()}
        files[CLASSIC_OWNER] = owner.read(CLASSIC_OWNER)
    manifest = json.loads(files['manifest.json'])
    old_hash = manifest['files'][CLASSIC_OWNER]
    new_hash = sha256(files[CLASSIC_OWNER]).hexdigest()
    old_line = f'{old_hash}  {CLASSIC_OWNER}\n'.encode('ascii')
    if files['payload.sha256'].count(old_line) != 1:
        raise ValueError('Unexpected Classic Picker launcher inventory')
    files['payload.sha256'] = files['payload.sha256'].replace(
        old_line, f'{new_hash}  {CLASSIC_OWNER}\n'.encode('ascii'), 1)
    manifest['files'][CLASSIC_OWNER] = new_hash
    files['manifest.json'] = (json.dumps(manifest, sort_keys=True) + '\n').encode()
    output = BytesIO()
    with ZipFile(output, 'w') as archive:
        for name, data in sorted(files.items()):
            archive.writestr(ZipInfo(name), data, compress_type=ZIP_DEFLATED)
    return output.getvalue()


def build(classic, classic_owner, max_variant, output):
    profiles = {}
    payload = {}
    for profile, path in (('classic', classic), ('max', max_variant)):
        version, expected = VARIANTS[profile]
        data = path.read_bytes()
        digest = sha256(data).hexdigest()
        if digest != expected:
            raise ValueError(f'Unexpected Picker {profile} variant digest')
        if profile == 'classic':
            data = upgrade_classic_variant(data, classic_owner.read_bytes())
            digest = sha256(data).hexdigest()
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
    parser.add_argument('classic_owner_zip', type=Path)
    parser.add_argument('max_zip', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(build(args.classic_zip, args.classic_owner_zip, args.max_zip, args.output))
