#include <iostream>

#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#ifdef _DEBUG
#include <d3d12sdklayers.h>
#include <dxgidebug.h>
#endif

int main(int, char**){
    std::cout << "Hello, from learn_d3d12!" << std::endl;

    POINT pt;
    GetCursorPos(&pt);
    std::cout << pt.x << " " << pt.y << std::endl;
}
