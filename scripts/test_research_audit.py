import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('research_audit', Path(__file__).with_name('research_audit.py'))
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class BrowserAuditTests(unittest.TestCase):
    def package(self):
        content = (b'exact event 9007199254740993\n' * 1200)
        return {'format': 'exsim.browser-audit/1', 'inventory': {
            'schemaVersion': 'exsim.audit-export/1', 'reproductionSupported': True,
            'items': [{'artifactIndex': '0', 'name': 'runner/events.jsonl', 'present': True,
                       'sizeBytes': str(len(content)), 'sha256': hashlib.sha256(content).hexdigest()}]},
                'artifacts': [{'artifactIndex': '0', 'contentHex': content.hex()}]}

    def test_complete_unpack_preserves_over_one_thousand_rows_and_detects_later_corruption(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory) / 'input.json'
            output = Path(directory) / 'audit'
            package.write_text(json.dumps(self.package()))
            audit.unpack(package, output)
            self.assertEqual(len((output / 'runner/events.jsonl').read_bytes().splitlines()), 1200)
            (output / 'runner/events.jsonl').write_bytes(b'corrupt')
            with self.assertRaises(ValueError):
                audit.verify(output)

    def test_unsafe_duplicate_missing_and_corrupt_inputs_never_create_package(self):
        for mode in ['unsafe', 'duplicate', 'missing', 'corrupt']:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                value = self.package()
                if mode == 'unsafe':
                    value['inventory']['items'][0]['name'] = '../escape'
                elif mode == 'duplicate':
                    value['artifacts'].append(value['artifacts'][0])
                elif mode == 'missing':
                    value['artifacts'] = []
                else:
                    value['artifacts'][0]['contentHex'] = '00'
                package = Path(directory) / 'input.json'
                output = Path(directory) / 'audit'
                package.write_text(json.dumps(value))
                with self.assertRaises((ValueError, KeyError)):
                    audit.unpack(package, output)
                self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
