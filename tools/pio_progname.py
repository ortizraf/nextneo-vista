# PlatformIO pre-script: names the firmware after the app (nextneo-vista.bin / .elf)
# instead of the generic firmware.bin.
Import("env")
env.Replace(PROGNAME="nextneo-vista")
