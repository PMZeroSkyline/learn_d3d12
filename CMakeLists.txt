cmake_minimum_required(VERSION 3.10.0)
project(learn_d3d12 VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17) # std::filesystem(c++17)

add_definitions(-DUNICODE -D_UNICODE)

include_directories(
    source
    extern/D3D12MemoryAllocator/include
    extern/D3D12MemoryAllocator/src
)
file(GLOB_RECURSE D3D12MA_SRC
    extern/D3D12MemoryAllocator/src/*.cpp
)
add_executable(learn_d3d12
    # extern/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp
    # ${D3D12MA_SRC}
    source/bindless_6_6.cpp
)
target_link_libraries(learn_d3d12 
    d3d12 
    dxgi
    dxguid
    d3dcompiler 
    user32 
    gdi32
    shlwapi.lib # for D3D12Sample StrStrI
)

