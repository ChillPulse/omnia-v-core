Import("env")
# PlatformIO's SCons application linker ignores CMake target_link_options.
# Append only to the SCons link command; bootloader source remains non-LTO.
env.Append(LINKFLAGS=["-flto"])
