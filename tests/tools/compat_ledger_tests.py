#!/usr/bin/env python3
"""Checks that tools/compat_ledger.py accepts the committed ledger inputs and
rejects inputs with the defects its validation exists to catch."""
import copy
import json
import os
import shutil
import sys
import tempfile
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
        # The boat trace fails in Circuit: it cannot back a compared claim alone.
        self.assert_rejected(lambda i: self.mechanic(i, "V21", "boats").update(cases=["trace/boat_motion"]), "needs a passing case")

    def test_mismatch_needs_a_failing_case(self):
        self.assert_rejected(lambda i: self.mechanic(i, "V14", "dust").update(evidence="mismatch"), "needs a failing case")

    def test_compared_evidence_needs_an_implementation(self):
        def mutate(i):
            m = self.mechanic(i, "V17", "other_explosions")
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

    def test_pack_formats_disagreeing_with_the_bundle(self):
        self.assert_rejected(lambda i: i["manifests"]["java-1.21.8"]["pack_formats"].update(data=80), "pack formats differ")

    def test_server_jar_disagreeing_with_the_bundle(self):
        def mutate(i):
            for r in i["manifests"]["java-1.21.4"]["reference_artifacts"]:
                if r["kind"] == "server":
                    r["sha1"] = "0" * 40
        self.assert_rejected(mutate, "server jar SHA-1 differs")

    def test_unclassified_resource(self):
        def mutate(i):
            i["families"]["classification"]["entity"] = [r for r in i["families"]["classification"]["entity"] if r[1] != ".*"]
        self.assert_rejected(mutate, "no classification rule matches")


class ReferencedPackets(unittest.TestCase):
    def test_lookups_are_found_in_subdirectories(self):
        with tempfile.TemporaryDirectory(prefix="compat-ledger-tests-") as tmp:
            os.makedirs(os.path.join(tmp, "net", "play"))
            with open(os.path.join(tmp, "top.cpp"), "w", encoding="utf-8") as f:
                f.write('auto a = cb(State::play, "keep_alive");\n')
            with open(os.path.join(tmp, "net", "play", "deep.cpp"), "w", encoding="utf-8") as f:
                f.write('auto b = sb(State::play, "chat");\nauto c = clientbound(State::login, "hello");\n')
            with open(os.path.join(tmp, "net", "notes.txt"), "w", encoding="utf-8") as f:
                f.write('cb(State::play, "ignored")\n')
            self.assertEqual(compat_ledger.referenced_packets(tmp),
                             {"play/clientbound/keep_alive", "play/serverbound/chat", "login/clientbound/hello"})

    def test_the_server_sources_reference_packets(self):
        self.assertIn("play/clientbound/keep_alive", compat_ledger.referenced_packets())


class LedgerSources(unittest.TestCase):
    """The per-entry source files: what read_inputs() refuses and how it orders entries."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="compat-ledger-tests-")
        self.compat = os.path.join(self.tmp, "compatibility")
        os.mkdir(self.compat)
        for name in sorted(os.listdir(compat_ledger.COMPAT)):
            source = os.path.join(compat_ledger.COMPAT, name)
            if name in ("inventory", "versions"):
                os.symlink(source, os.path.join(self.compat, name))
            elif os.path.isdir(source):
                shutil.copytree(source, os.path.join(self.compat, name))
            elif name != "ledger.json":
                shutil.copy(source, self.compat)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def write(self, rel, data):
        path = os.path.join(self.compat, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(data if isinstance(data, str) else compat_ledger.canonical(data))

    def read(self, rel):
        with open(os.path.join(self.compat, rel), encoding="utf-8") as f:
            return json.load(f)

    def errors(self):
        try:
            inputs = compat_ledger.read_inputs(self.compat)
        except compat_ledger.Failure as e:
            return [str(e)]
        return compat_ledger.produce(inputs)[2]

    def assert_rejected(self, fragment):
        errors = self.errors()
        self.assertTrue(any(fragment in e for e in errors), f"expected an error containing {fragment!r}, got {errors[:5]}")

    def test_copied_sources_are_valid(self):
        self.assertEqual(self.errors(), [])

    def test_sources_join_into_the_documents(self):
        inputs = compat_ledger.read_inputs(self.compat)
        families = inputs["families"]["families"]
        self.assertEqual([f["id"] for f in families], [f"V{i:02d}" for i in range(1, 37)])
        self.assertEqual(families[13]["mechanics"][0]["id"], self.read("families/V14/_family.json")["mechanics"][0])
        self.assertEqual(inputs["resources"]["overrides"]["packet"]["play/serverbound/container_click"],
                         self.read("resources/packet/play/serverbound/container_click.json"))
        self.assertIn("client/windows", [c["id"] for c in inputs["cases"]["cases"]])
        self.assertEqual(sorted(inputs["families"]["classification"]), sorted(compat_ledger.NAMED_KINDS))

    def test_an_unlisted_mechanic_follows_the_listed_ones_by_id(self):
        listed = self.read("families/V17/_family.json")["mechanics"]
        mechanic = {"name": "A new mechanic", "milestone": "C3", "impl": "missing", "evidence": "none"}
        self.write("families/V17/zz_new.json", mechanic)
        self.write("families/V17/aa_new.json", mechanic)
        inputs = compat_ledger.read_inputs(self.compat)
        ids = [m["id"] for m in next(f for f in inputs["families"]["families"] if f["id"] == "V17")["mechanics"]]
        self.assertEqual(ids, listed + ["aa_new", "zz_new"])
        ledger, _, errors = compat_ledger.produce(inputs)
        self.assertEqual(errors, [])
        self.assertIn("V17.zz_new", [f["id"] for f in ledger["features"]])

    def test_new_entries_need_no_other_file(self):
        self.write("cases/client/zz_new.json", {"kind": "planned", "versions": ["java-1.21.8"], "location": "none yet",
                                                "status": "planned", "description": "A new case"})
        self.write("resources/block/dirt.json", {"impl": "partial", "evidence": "none", "deviations": ["a gap"]})
        ledger, _, errors = compat_ledger.produce(compat_ledger.read_inputs(self.compat))
        self.assertEqual(errors, [])
        self.assertIn("client/zz_new", [c["id"] for c in ledger["cases"]])
        dirt = next(f for f in ledger["features"] if f["id"] == "block/minecraft:dirt")
        self.assertEqual(dirt["versions"]["java-1.21.8"]["impl"], "partial")

    def test_listed_mechanic_without_a_file(self):
        os.remove(os.path.join(self.compat, "families/V17/tnt_block.json"))
        self.assert_rejected("mechanic tnt_block is listed but has no file")

    def test_mechanic_listed_twice(self):
        head = self.read("families/V17/_family.json")
        head["mechanics"].append(head["mechanics"][0])
        self.write("families/V17/_family.json", head)
        self.assert_rejected("is listed twice")

    def test_key_given_twice(self):
        # What a merge of two branches adding the same key on different lines leaves behind.
        self.write("families/V17/tnt_block.json", '{\n  "name": "a",\n  "milestone": "C3",\n  "impl": "missing",\n  "impl": "partial"\n}\n')
        self.assert_rejected("families/V17/tnt_block.json: key impl is given twice")

    def test_file_that_is_not_json(self):
        self.write("cases/client/windows.json", "<<<<<<< ours\n{}\n")
        self.assert_rejected("compatibility/cases/client/windows.json:")

    def test_file_not_in_canonical_form(self):
        data = self.read("cases/client/windows.json")
        self.write("cases/client/windows.json", json.dumps(data))
        self.assert_rejected("cases/client/windows.json: not in canonical form")

    def test_unexpected_file(self):
        self.write("resources/block/stone.json.orig", "{}\n")
        self.assert_rejected("resources/block/stone.json.orig: unexpected file")

    def test_badly_named_entry(self):
        self.write("families/V17/Bad-Name.json", {"name": "a", "milestone": "C3", "impl": "missing"})
        self.assert_rejected("families/V17/Bad-Name.json: unexpected file")

    def test_mechanic_in_a_subdirectory(self):
        self.write("families/V17/sub/thing.json", {"name": "a", "milestone": "C3", "impl": "missing"})
        self.assert_rejected("mechanics are files of the family's directory")

    def test_unknown_resource_kind(self):
        os.mkdir(os.path.join(self.compat, "resources", "gadget"))
        self.assert_rejected("its directories must be the kinds")

    def test_case_file_defining_a_trace(self):
        self.write("cases/trace/vertical_slice.json", {"kind": "official_trace", "versions": ["java-1.21.8"], "location": "x",
                                                       "status": "passing", "description": "x"})
        self.assert_rejected("defined twice")


if __name__ == "__main__":
    unittest.main(verbosity=1)
