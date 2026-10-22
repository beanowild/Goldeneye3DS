# 3DS-native-windows.cmake - devkitPro's 3DS toolchain for a native
# (non-MSYS2) CMake on Windows.
#
# devkitPro's own entry point (3DS.cmake -> devkitARM.cmake ->
# dkp-initialize-path.cmake) refuses to run unless CMake itself came from
# MSYS2. The toolchain doesn't need MSYS2: the compilers and tools are native
# Windows programs, and the platform file only builds ordinary commands.
# This file does what those three entry files do, minus that guard, then
# hands over to devkitPro's unmodified Platform/Nintendo3DS.cmake and
# helper modules. Used by builder.py on Windows with the toolchain from
# tools/fetch_toolchain.py.
#
# Requires the DEVKITPRO environment variable (builder.py sets it).

cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED ENV{DEVKITPRO})
	message(FATAL_ERROR "Set DEVKITPRO to the devkitPro directory (builder.py does this)")
endif()
file(TO_CMAKE_PATH "$ENV{DEVKITPRO}" DEVKITPRO)

list(APPEND CMAKE_MODULE_PATH "${DEVKITPRO}/cmake")

# --- 3DS.cmake / devkitARM.cmake, without dkp-initialize-path's guard ---
set(CMAKE_SYSTEM_NAME Nintendo3DS)
set(CMAKE_SYSTEM_PROCESSOR armv6k)

include(dkp-toolchain-common)
set(DKP_BIN2S_ALIGNMENT 4)
# The macro below finds this relative to the *calling* file, i.e. us.
set(CMAKE_USER_MAKE_RULES_OVERRIDE "${DEVKITPRO}/cmake/dkp-rule-overrides.cmake")
__dkp_toolchain(devkitARM arm arm-none-eabi)

# The macro splits $PATH on ':', which on Windows mangles drive letters
# ("C:\..."). Rebuild the program search path with Windows separators.
file(TO_CMAKE_PATH "$ENV{PATH}" CMAKE_SYSTEM_PROGRAM_PATH)

set(CTR_ROOT ${DEVKITPRO}/libctru)
set(DKP_INSTALL_PREFIX_INIT ${DEVKITPRO}/portlibs/3ds)
__dkp_platform_prefix(
	${DEVKITPRO}/portlibs/3ds
	${CTR_ROOT}
)

# 3DS.cmake insists on arm-none-eabi-pkg-config, a shell script that native
# Windows can't run. This project doesn't use pkg-config, so point the
# variable at the script to satisfy the check without running it.
set(PKG_CONFIG_EXECUTABLE "${DEVKITPRO}/portlibs/3ds/bin/arm-none-eabi-pkg-config"
	CACHE FILEPATH "unused on native Windows")

find_program(CTR_SMDHTOOL_EXE NAMES smdhtool HINTS "${DEVKITPRO}/tools/bin")
find_program(CTR_3DSXTOOL_EXE NAMES 3dsxtool HINTS "${DEVKITPRO}/tools/bin")
find_program(CTR_PICASSO_EXE  NAMES picasso  HINTS "${DEVKITPRO}/tools/bin")
find_program(CTR_TEX3DS_EXE   NAMES tex3ds   HINTS "${DEVKITPRO}/tools/bin")
find_file(CTR_DEFAULT_ICON NAMES default_icon.png HINTS "${CTR_ROOT}" NO_CMAKE_FIND_ROOT_PATH)
