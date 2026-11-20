#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct DisplayTarget {
    HMONITOR monitor{};
    RECT bounds{};
    std::wstring device,identity,name,label;
    bool primary{};
};
std::vector<DisplayTarget> EnumerateDisplays();
RECT ParkOutsideDisplays(const std::vector<RECT>& displays,LONG width,LONG height,LONG preferredX);
bool IntersectsAnyDisplay(const RECT& rectangle,const std::vector<RECT>& displays);
