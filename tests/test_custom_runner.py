"""PlatformIO custom test runner for the native host tests.

The native test program (`tests/cpp/test_behavior.cpp`) is a plain C++ binary
that runs a series of `assert`-based checks and prints `behavior tests passed`
on success. A failing assert aborts the process with a non-zero / signal exit
code, which PlatformIO's native reader turns into a test error.

This runner simply:
  * echoes program output,
  * records a PASSED case when the success marker is seen,
  * records a FAILED case if the program finishes without that marker.

`test_framework = custom` in `platformio.ini` points PlatformIO here.
"""

import click

from platformio.test.result import TestCase, TestStatus
from platformio.test.runners.base import TestRunnerBase

SUCCESS_MARKER = "behavior tests passed"


class CustomTestRunner(TestRunnerBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._passed = False

    def on_testing_line_output(self, line):
        if self.options.verbose:
            click.echo(line, nl=False)
        if SUCCESS_MARKER in line:
            self._passed = True
            self.test_suite.add_case(
                TestCase(
                    name="native_behavior",
                    status=TestStatus.PASSED,
                    stdout=line.strip(),
                )
            )
            self.test_suite.on_finish()

    def teardown(self):
        # If the program exited without printing the success marker and no case
        # was recorded, surface it as a failure instead of an empty (skipped) run.
        if not self._passed and not self.test_suite.cases:
            self.test_suite.add_case(
                TestCase(
                    name="native_behavior",
                    status=TestStatus.FAILED,
                    message="Native test program did not report success",
                )
            )
