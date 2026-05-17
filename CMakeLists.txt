cmake_minimum_required(VERSION 3.20)
project(linnaria LANGUAGES C CXX)

find_package(PkgConfig REQUIRED)

# Dynarmic requires C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable compile_commands.json building
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# set only A32 frontend building since we don't need A64
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "Selects which frontends to enable")

# 1. Bring in Dynarmic (Assuming you cloned it into third-party/dynarmic)
add_subdirectory(third-party/dynarmic)

# 2. Find SDL2 on your Linux host
find_package(SDL2 REQUIRED)
find_package(ZLIB REQUIRED)
#find_package(OpenGL REQUIRED)

pkg_check_modules(GLES2 REQUIRED IMPORTED_TARGET glesv2)

# 3. Create your executable
add_executable(linnaria
    src/main.cpp
    src/ElfLoader.cpp
    src/ThreadingHelpers.cpp
    glad/gles2.c
)

#target_compile_features(linnaria PRIVATE cxx_std_20)
# ASAN
#target_compile_options(linnaria PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g)
#target_link_options(linnaria PRIVATE -fsanitize=thread)

# 4. Link everything together
target_include_directories(linnaria PRIVATE src glad .)
target_link_libraries(linnaria PRIVATE 
    dynarmic
    PkgConfig::GLES2
    SDL2::SDL2
    ZLIB::ZLIB
)
