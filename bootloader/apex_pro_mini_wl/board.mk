MCU_SUB_VARIANT = nrf52833
SD_VERSION=7.2.0

# Enable A/B recovery support. This code can rewrite the application slot, so
# deploy bootloader changes through SWD. The upstream build collects sources
# from C_SRC.
CFLAGS += -DAPEX_ENABLE_AB_ROLLBACK
C_SRC += src/ab_promote.c
