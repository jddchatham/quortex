import unittest

from quortex.autonomy.inference_service import validate


class InferenceServiceValidationTests(unittest.TestCase):
    def base(self):
        return {
            "request_id": "request-123456",
            "operation": "two_token_reference",
            "model": "/home/user/model.gguf",
            "output": "/home/user/quortex/out",
            "evidence": "/home/user/quortex/evidence.json",
            "index": "/home/user/index.json",
        }

    def test_allowlisted_request_maps_to_fixed_target(self):
        target, values = validate(self.base())
        self.assertEqual(target, "reference")
        self.assertEqual(values[0], "reference")

    def test_arbitrary_operation_rejected(self):
        doc = self.base(); doc["operation"] = "shell"
        with self.assertRaises(ValueError): validate(doc)

    def test_unexpected_command_field_rejected(self):
        doc = self.base(); doc["command"] = "/bin/sh"
        with self.assertRaises(ValueError): validate(doc)

    def test_malformed_request_id_rejected(self):
        doc = self.base(); doc["request_id"] = "../../escape"
        with self.assertRaises(ValueError): validate(doc)
