MCU_SUB_VARIANT = nrf52833

# Dongle board: no external NOR, so no A/B rollback (that support is gated on the
# keyboard board in CMakeLists.txt). No extra C_SRC beyond the per-board
# pinconfig.c that the build already compiles.
