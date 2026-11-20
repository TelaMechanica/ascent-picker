#!/usr/bin/env python3
"""Build only the exact classic Gnd485 Picker owner; does not contact hardware."""
from pathlib import Path
import argparse, hashlib, json, subprocess
ROOT=Path(__file__).resolve().parents[1]
MODULES='ascent_picker_rt ascent_picker_ctl ascent_cells ascent_switch ascent_sw_rpc ascent_rc_source ascent_crsf ascent_mavlink ascent_geo ascent_ring ascent_picker_cfg ascent_picker_ipc ascent_picker_osd'.split()
def build(libc,output,compiler='aarch64-linux-gnu-gcc'):
    libc_sha256 = '9e17e7a1f88f7fd5767606dd9f75dd8e457599b2a02d9195155912ee87970db5'
    if hashlib.sha256(libc.read_bytes()).hexdigest()!=libc_sha256:
        raise ValueError('Exact native libc preimage required')
    output.mkdir(parents=True,exist_ok=False)
    binary=output/'ascent_picker_rt';src=ROOT/'vrx/map_runtime/src'
    cmd=[compiler,'-O2','-DASCENT_CLASSIC_PAIR_RELEASE','-std=gnu11','-ffreestanding','-fno-builtin-memcpy','-mno-outline-atomics','-fvisibility=hidden','-ffunction-sections','-fdata-sections','-Wall','-Wextra','-Werror','-Wno-unused-function','-fPIE','-o',str(binary)]
    cmd += [str(src/(name+'.c'))for name in MODULES]
    cmd += ['-nostdlib','-Wl,--gc-sections','-Wl,-z,max-page-size=4096','-Wl,--build-id=none','-Wl,-pie','-Wl,-e,_start','-Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1',str(libc)]
    subprocess.run(cmd,check=True)
    record={'profile':'native-gnd485-19.2.14','classic_release':True,'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'libc_sha256':libc_sha256,'sources':{p.relative_to(ROOT).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()for p in [src/(name+'.c')for name in MODULES]+sorted(src.glob('*.h'))}}
    (output/'build.json').write_text(json.dumps(record,indent=2)+'\n')
    return record
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--libc',required=True,type=Path);parser.add_argument('--output',required=True,type=Path);parser.add_argument('--compiler',default='aarch64-linux-gnu-gcc');args=parser.parse_args()
    print(json.dumps(build(args.libc.resolve(),args.output.resolve(),args.compiler)))
