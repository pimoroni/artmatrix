import cppmem

# Switch C++ memory allocations to use MicroPython's heap
cppmem.set_mode(cppmem.MICROPYTHON)

import artmatrix  # noqa F401

if file_exists("hardware_test.txt"):
    import hardware_test  # noqa F401

# Defer to user boot if it's present
if file_exists("/boot.py"):
    __import__("/boot.py")
