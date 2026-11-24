import importlib.util
import sys
import unittest


spec = importlib.util.spec_from_file_location("gesture_paj7620", sys.argv.pop(1))
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)
LEFT, UP, CLOCKWISE = 0x002, 0x004, 0x040


class GestureSequenceTest(unittest.TestCase):
    def setUp(self):
        self.sequence = driver.GestureSequence()

    def test_exact_order_completes_once(self):
        self.assertEqual(self.sequence.accept(LEFT, 1), 1)
        self.assertEqual(self.sequence.accept(UP, 2), 2)
        self.assertEqual(self.sequence.accept(CLOCKWISE, 3), 3)
        self.assertEqual(self.sequence.accept(CLOCKWISE, 4), 0)

    def test_wrong_or_combined_flags_reset_progress(self):
        self.assertEqual(self.sequence.accept(UP, 1), 0)
        self.assertEqual(self.sequence.accept(LEFT, 2), 1)
        self.assertEqual(self.sequence.accept(UP | CLOCKWISE, 3), 0)
        self.assertEqual(self.sequence.accept(CLOCKWISE, 4), 0)

    def test_timeout_and_restart(self):
        self.assertEqual(self.sequence.accept(LEFT, 1), 1)
        self.assertTrue(self.sequence.expire(6))
        self.assertEqual(self.sequence.accept(UP, 6), 0)
        self.assertEqual(self.sequence.accept(LEFT, 7), 1)
        self.assertEqual(self.sequence.accept(LEFT, 8), 1)
        self.assertEqual(self.sequence.accept(UP, 12), 2)
        self.assertEqual(self.sequence.accept(CLOCKWISE, 17), 0)

    def test_return_motion_does_not_reset_or_count(self):
        self.assertEqual(self.sequence.accept(LEFT, 1), 1)
        self.assertEqual(self.sequence.accept(0x001, 1.1), 1)
        self.assertEqual(self.sequence.accept(UP, 1.2), 1)
        self.assertEqual(self.sequence.accept(UP, 1.5), 2)
        self.assertEqual(self.sequence.accept(0x008, 1.6), 2)
        self.assertEqual(self.sequence.accept(CLOCKWISE, 2), 3)

    def test_return_motion_does_not_extend_deadline(self):
        self.assertEqual(self.sequence.accept(LEFT, 1), 1)
        self.assertEqual(self.sequence.accept(0x001, 1.1), 1)
        self.assertEqual(self.sequence.accept(UP, 6), 0)


if __name__ == "__main__":
    unittest.main()
