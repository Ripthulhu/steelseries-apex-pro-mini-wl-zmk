"""Force Zephyr's module scanner into its explicit-module, no-west path.

The checked-out west manifest cannot resolve Zephyr's manifest-rev import, but
all module paths required for this reproducible offline build are supplied via
ZEPHYR_MODULES.  Raising ImportError here prevents zephyr_module.py from trying
to parse that unrelated broken west import.
"""

raise ImportError("G4A offline build uses an explicit ZEPHYR_MODULES list")

