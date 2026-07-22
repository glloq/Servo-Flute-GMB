# The PlatformIO native test runner (test_custom_runner.py) matches pytest's
# test_*.py pattern but is not a pytest module - it is imported by PlatformIO.
# Skip it during pytest collection to avoid importing platformio unnecessarily.
collect_ignore = ["test_custom_runner.py"]
