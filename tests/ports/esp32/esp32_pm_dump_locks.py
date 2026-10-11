try:
    import esp32
    from errno import EOPNOTSUPP

    dump_locks = esp32.pm_dump_locks
except (ImportError, AttributeError):
    print("SKIP")
    raise SystemExit

try:
    locks = dump_locks()
except OSError as error:
    # Custom builds may disable PM. Other failures must remain test failures.
    if error.errno != EOPNOTSUPP:
        raise
    print("SKIP")
    raise SystemExit
print(isinstance(locks, str))
print("Lock stats:" in locks)
print("Active" in locks)
