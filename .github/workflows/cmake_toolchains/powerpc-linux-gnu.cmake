set(CMAKE_SYSTEM_NAME Linux)
set(TOOLCHAIN_PREFIX powerpc-linux-gnu)

set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)