"""Independent host projection/native parser contracts; generated identities only."""
from pathlib import Path
import ctypes
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
import cells_projection as geometry
import picker_projection as projection


def identity(seed):
    return ':'.join(f'{byte:02X}' for byte in (0x12000000+seed).to_bytes(4,'big'))


class ProjectionContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.owned=tempfile.TemporaryDirectory(prefix='picker-projection-')
        target=Path(cls.owned.name)/'bridge.so'
        subprocess.run(['gcc','-shared','-fPIC','-O2','-std=gnu11','-DASCENT_HOSTED',
            '-ffile-prefix-map='+str(ROOT)+'=/candidate','-Ivrx/map_runtime/src',
            'vrx/map_runtime/src/ascent_cells.c','vrx/map_runtime/src/ascent_picker_cfg.c',
            'vrx/map_runtime/src/ascent_switch.c',
            'tests/config_bridge.c','-lm','-o',str(target)],cwd=ROOT,check=True,capture_output=True)
        cls.native=ctypes.CDLL(str(target))
        cls.native.a_cells_center_us.argtypes=[ctypes.c_int,ctypes.c_int]
        cls.native.a_cells_center_us.restype=ctypes.c_int
        cls.native.fixture_config_field.argtypes=[ctypes.c_char_p,ctypes.c_size_t,ctypes.c_int]
        cls.native.fixture_config_field.restype=ctypes.c_int
        cls.native.fixture_config_id.argtypes=[ctypes.c_char_p,ctypes.c_size_t,ctypes.c_int,ctypes.c_int]
        cls.native.fixture_config_id.restype=ctypes.c_int

    @classmethod
    def tearDownClass(cls):cls.owned.cleanup()

    def test_read_only_centers_match_native_for_every_supported_host_cell(self):
        for count in range(1,9):
            for cell in range(1,count+1):
                with self.subTest(count=count,cell=cell):
                    self.assertEqual(geometry.center_us(cell,count),self.native.a_cells_center_us(cell,count))
        self.assertFalse(hasattr(geometry,'classify'))
        self.assertFalse(hasattr(projection,'select'))

    def test_name_alone_is_not_receiver_membership(self):
        rows=[{'bb_id':identity(1),'nickname':'Fixture A'},{'bb_id':identity(2),'nickname':'Fixture B'}]
        result=projection.build_projection(rows,[identity(1)])
        self.assertEqual(result['active_valid_count'],1);self.assertFalse(result['enabled'])
        self.assertFalse(result['cells'][1]['valid'])
        self.assertEqual([c['bb_id']for c in projection.deployable_config(result)['cells']],[identity(1)])

    def test_two_members_and_timing_fields_roundtrip_through_native_parser(self):
        ids=[identity(1),identity(2)];value=projection.build_projection([{'bb_id':v}for v in ids],ids)
        encoded=projection.serialize_config(value,hold_ms=345,rc_channel=7,rc_enabled=True,visible=False).encode()
        self.assertTrue(value['enabled'])
        for field,expected in enumerate([2,345,7,1,1]):
            self.assertEqual(self.native.fixture_config_field(encoded,len(encoded),field),expected)
        for cell,value in enumerate(ids):
            expected=bytes.fromhex(value.replace(':',''))
            self.assertEqual(bytes(self.native.fixture_config_id(encoded,len(encoded),cell,byte)for byte in range(4)),expected)

    def test_invalid_cells_never_become_native_targets(self):
        ids=[identity(1),identity(2),identity(3)]
        value=projection.build_projection([{'bb_id':v}for v in ids],[ids[0],ids[2]])
        encoded=projection.serialize_config(value).encode()
        self.assertEqual(value['configured_count'],3)
        self.assertEqual(value['centers'],[self.native.a_cells_center_us(i,3) for i in range(1,4)])
        self.assertEqual(self.native.fixture_config_field(encoded,len(encoded),0),3)
        self.assertIn(b'\nempty\n',encoded)
        self.assertNotIn(ids[1].encode(),encoded)
        self.assertEqual([self.native.fixture_config_id(encoded,len(encoded),1,i) for i in range(4)],[0]*4)
        self.assertEqual(bytes(self.native.fixture_config_id(encoded,len(encoded),2,i) for i in range(4)),bytes.fromhex(ids[2].replace(':','')))

    def test_duplicate_ids_cannot_be_serialized_as_two_valid_aircraft(self):
        value=projection.build_projection([{'bb_id':identity(1)},{'bb_id':identity(1)}],[identity(1)])
        with self.assertRaises(ValueError):projection.serialize_config(value)

    def test_label_newlines_cannot_inject_configuration_directives(self):
        ids=[identity(1),identity(2)]
        value=projection.build_projection([{'bb_id':v,'nickname':'Fixture\nrc_channel 1'}for v in ids],ids)
        encoded=projection.serialize_config(value,rc_channel=7).encode()
        self.assertEqual(self.native.fixture_config_field(encoded,len(encoded),2),7)
        self.assertEqual(self.native.fixture_config_field(encoded,len(encoded),0),2)

    def test_sentinels_and_missing_membership_do_not_enable_configuration(self):
        sentinel=':'.join(['00']*4)
        value=projection.build_projection([{'bb_id':sentinel},{'bb_id':identity(1)}],[])
        self.assertFalse(value['enabled']);self.assertEqual(value['active_valid_count'],0)


if __name__=='__main__':unittest.main()
