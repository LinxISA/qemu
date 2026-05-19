# Default configuration for linx64-softmmu

# Keep the linx64 alias behavior identical to the canonical linx-softmmu
# device set so rebuilt qemu-system-linx64 matches the historical lane.
CONFIG_SEMIHOSTING=y
CONFIG_ARM_COMPATIBLE_SEMIHOSTING=y

# Boards:
CONFIG_LINX_VIRT=y
