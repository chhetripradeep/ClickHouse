import unittest

from contract import BATCHES, Attempt, batch_rows, check_rows


class ContractTest(unittest.TestCase):
    def test_success_survives_later_unknown_attempt(self):
        attempts = [Attempt(3, "r1", "a", "success"), Attempt(3, "r2", "b", "unknown")]
        rows = batch_rows("run", 3)
        self.assertEqual(check_rows("run", attempts, rows), [])
        self.assertTrue(check_rows("run", attempts, rows[1:]))
        self.assertTrue(check_rows("run", attempts, []))

    def test_unknown_can_be_absent_or_complete_but_not_partial(self):
        attempts = [Attempt(7, "r1", "a", "unknown")]
        rows = batch_rows("run", 7)
        self.assertEqual(check_rows("run", attempts, []), [])
        self.assertEqual(check_rows("run", attempts, rows), [])
        self.assertTrue(check_rows("run", attempts, rows[:-1]))

    def test_equal_counts_do_not_hide_bad_contents(self):
        attempts = [Attempt(2, "r1", "a", "success")]
        rows = batch_rows("run", 2)
        self.assertTrue(check_rows("run", attempts, rows[:-1] + [rows[0]]))
        bad_payload = (*rows[0][:3], "wrong")
        self.assertTrue(check_rows("run", attempts, [bad_payload] + rows[1:]))
        self.assertTrue(check_rows("run", attempts, rows + batch_rows("run", 4)))

    def test_final_reconciliation_requires_every_batch(self):
        attempts = [Attempt(i, "r1", str(i), "success") for i in range(BATCHES)]
        rows = [row for i in range(BATCHES) for row in batch_rows("run", i)]
        self.assertEqual(check_rows("run", attempts, rows, require_all=True), [])
        self.assertTrue(check_rows("run", attempts, rows[:-16], require_all=True))


if __name__ == "__main__":
    unittest.main()
