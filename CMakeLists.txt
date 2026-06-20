cmake_minimum_required(VERSION 3.10.0)
project(learn_d3d12 VERSION 0.1.0 LANGUAGES C CXX)

add_definitions(-DUNICODE -D_UNICODE)

include_directories(
    source
)
# file(GLOB_RECURSE SRC
#     source/*.cpp
# )
# add_executable(learn_d3d12
#     ${SRC}
# )
add_executable(learn_d3d12
    source/texture.cpp
)
target_link_libraries(learn_d3d12 
    d3d12 
    dxgi
    dxguid
    d3dcompiler 
    user32 
    gdi32
)

