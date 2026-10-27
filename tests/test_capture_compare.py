import struct, tempfile, unittest
from pathlib import Path
from tools.compare_captures import BOUNDARY, ROUTER, PRE_HC, POST_SEGMENT_B, main

class CaptureCompareTest(unittest.TestCase):
    def test_synthetic_identical(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); ref=root/'r'; quo=root/'q'; ref.mkdir(); quo.mkdir()
            for name,n,prefix in [('segment_a_boundary',BOUNDARY,'cortex'),
                                  ('router_logits',ROUTER,'cortex'),
                                  ('pre_hc_block_out',PRE_HC,'cortex'),
                                  ('post_segment_b',POST_SEGMENT_B,'cortex')]:
                data=struct.pack('<%df'%n, *([1.0]*n)); (ref/f'{prefix}_{name}_p0.bin').write_bytes(data)
                (quo/f'quortex_{name}_p0.bin').write_bytes(data)
            import sys
            old=sys.argv; sys.argv=['compare',str(ref),str(quo)]
            try: self.assertEqual(main(),0)
            finally: sys.argv=old

    def test_pre_hc_mismatch_is_reported(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); ref=root/'r'; quo=root/'q'; ref.mkdir(); quo.mkdir()
            for name,n in [('segment_a_boundary',BOUNDARY),('router_logits',ROUTER),
                           ('pre_hc_block_out',PRE_HC),('post_segment_b',POST_SEGMENT_B)]:
                (ref/f'cortex_{name}_p0.bin').write_bytes(struct.pack('<%df'%n, *([1.0]*n)))
                values=[1.0]*n
                if name == 'pre_hc_block_out': values[17] = 2.0
                (quo/f'quortex_{name}_p0.bin').write_bytes(struct.pack('<%df'%n, *values))
            import sys
            old=sys.argv; sys.argv=['compare',str(ref),str(quo)]
            try: self.assertEqual(main(),1)
            finally: sys.argv=old

if __name__ == '__main__': unittest.main()
