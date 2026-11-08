import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock


class CleanupTests(unittest.TestCase):
    def test_report_and_other_resources_survive_adapter_close_error(self):
        from tools.hardware_check import finish_report
        report = {'errors': []}
        broken = Mock(side_effect=OSError('adapter removed'))
        other = Mock()
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'result.json'
            with self.assertRaises(RuntimeError):
                finish_report(report, path, [('CAN', broken), ('USB', other)])
            other.assert_called_once()
            result = json.loads(path.read_text())
            self.assertIn('adapter removed', result['cleanup_errors'][0])

