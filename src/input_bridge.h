#pragma once
#include <windows.h>
#include <atomic>
#include <thread>

// Maps an output-client point through the displayed 16:9 viewport into the
// source game's client area. Rejects letterbox bars instead of clicking them.
bool MapOutputPoint(POINT point,SIZE output,SIZE source,POINT& mapped);

class InputBridge {
public:
    ~InputBridge() { Stop(); }
    void Start(HWND source,HWND output);
    void Stop();
    bool Acquire();
    void Release();
    bool Armed() const { return armed_.load(); }
    bool Confined() const { return confined_.load(); }
    bool Forward(UINT message,WPARAM wp,LPARAM lp);
    unsigned long long Forwarded() const { return forwarded_.load(); }
private:
    HWND source_{},output_{};
    DWORD sourcePid_{};
    RECT outputBounds_{};
    std::atomic<bool> armed_{false},confined_{false};
    std::atomic<unsigned long long> forwarded_{0};
    std::jthread worker_;
};
