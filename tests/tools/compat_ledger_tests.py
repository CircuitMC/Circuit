#!/usr/bin/env python3
"""Checks that tools/compat_ledger.py accepts the committed ledger inputs and
rejects inputs with the defects its validation exists to catch."""
import copy
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import compat_ledger  # noqa: E402


class LedgerValidation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.inputs = compat_ledger.read_inputs()

    def errors_for(self, mutate):
        inputs = copy.deepcopy(self.inputs)
        mutate(inputs)
        _, _, errors = compat_ledger.produce(inputs)
        return errors

    def assert_rejected(self, mutate, fragment):
        errors = self.errors_for(mutate)
        self.assertTrue(any(fragment in e for e in errors), f"expected an error containing {fragment!r}, got {errors[:5]}")

    def mechanic(self, inputs, family, mechanic):
        fam = next(f for f in inputs["families"]["families"] if f["id"] == family)
        return next(m for m in fam["mechanics"] if m["id"] == mechanic)

    def test_committed_inputs_are_valid(self):
        self.assertEqual(self.errors_for(lambda i: None), [])

    def test_every_enumerated_name_has_a_feature(self):
        ledger, _, _ = compat_ledger.produce(copy.deepcopy(self.inputs))
        ids = {f["id"] for f in ledger["features"]}
        for bundle, inv in self.inputs["inventories"].items():
            for name in inv["kinds"]["block"]["names"]:
                self.assertIn(f"block/minecraft:{name}", ids)

    def test_version_only_names_are_not_applicable_elsewhere(self):
        ledger, _, _ = compat_ledger.produce(copy.deepcopy(self.inputs))
        ghast = next(f for f in ledger["features"] if f["id"] == "entity/minecraft:happy_ghast")
        self.assertEqual(ghast["versions"]["java-1.21.4"]["impl"], "not_applicable")
        self.assertEqual(ghast["versions"]["java-1.21.8"]["impl"], "missing")

    def test_duplicate_case(self):
        self.assert_rejected(lambda i: i["cases"]["cases"].append(copy.deepcopy(i["cases"]["cases"][0])), "defined twice")

    def test_dangling_case(self):
        self.assert_rejected(lambda i: self.mechanic(i, "V17", "tnt_block").setdefault("cases", []).append("trace/no_such_trace"),
                             "dangling case")

    def test_dangling_planned_case(self):
        self.assert_rejected(lambda i: self.mechanic(i, "V17", "tnt_block")["cases_planned"].append("TNT-99"), "dangling planned case")

    def test_not_applicable_needs_a_reason(self):
        self.assert_rejected(lambda i: self.mechanic(i, "V28", "poi").update(impl="not_applicable"), "needs a reason")

    def test_compared_evidence_needs_a_case(self):
        self.assert_rejected(lambda i: self.mechanic(i, "V28", "poi").update(impl="partial", evidence="compared"), "needs a case")

    def test_compared_evidence_needs_a_passing_case(self):
        # The TNT trace is unsupported by Circuit: it cannot back a compared claim.
        self.assert_rejected(lambda i: self.mechanic(i, "V17", "tnt_block").update(impl="partial", evidence="compared"),
                             "needs a passing case")

    def test_mismatch_needs_a_failing_case(self):
        self.assert_rejected(lambda i: self.mechanic(i, "V14", "dust").update(evidence="mismatch"), "needs a failing case")

    def test_compared_evidence_needs_an_implementation(self):
        def mutate(i):
            m = self.mechanic(i, "V17", "tnt_block")
            m.update(evidence="compared", cases=["trace/vertical_slice"])
        self.assert_rejected(mutate, "compared evidence needs an implementation")

    def test_unknown_states(self):
        self.assert_rejected(lambda i: self.mechanic(i, "V17", "tnt_block").update(impl="done"), "implementation state")

    def test_duplicate_feature(self):
        def mutate(i):
            fam = next(f for f in i["families"]["families"] if f["id"] == "V17")
            fam["mechanics"].append(copy.deepcopy(fam["mechanics"][0]))
        self.assert_rejected(mutate, "duplicate feature")

    def test_missing_family(self):
        self.assert_rejected(lambda i: i["families"]["families"].pop(), "V01-V36")

    def test_override_of_an_unknown_resource(self):
        self.assert_rejected(lambda i: i["resources"]["overrides"]["block"].update({"no_such_block": {"impl": "partial"}}),
                             "not in any inventory")

    def test_manifest_disagreeing_with_the_bundle(self):
        self.assert_rejected(lambda i: i["manifests"]["java-1.21.4"].update(protocol=770), "differs from src/contracts/version.cpp")

    def test_unclassified_resource(self):
        def mutate(i):
            i["families"]["classification"]["entity"] = [r for r in i["families"]["classification"]["entity"] if r[1] != ".*"]
        self.assert_rejected(mutate, "no classification rule matches")


if __name__ == "__main__":
    unittest.main(verbosity=1)
