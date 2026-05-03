cmake_minimum_required(VERSION 3.20)
project(Linnaria LANGUAGES C CXX)

# Dynarmic requires C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Bring in Dynarmic (Assuming you cloned it into third-party/dynarmic)
add_subdirectory(third-party/dynarmic)

# 2. Find SDL2 on your Linux host
find_package(SDL2 REQUIRED)
find_package(ZLIB REQUIRED)

# 3. Create your executable
add_executable(Linnaria
    src/main.cpp
    src/ElfLoader.cpp
)

# 4. Link everything together
target_include_directories(Linnaria PRIVATE src)
target_link_libraries(Linnaria PRIVATE 
    dynarmic
    SDL2::SDL2
    ZLIB::ZLIB
)
