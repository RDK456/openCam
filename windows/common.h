#pragma once
// Shared between the virtual camera DLL (runs inside Windows Frame Server) and the tray app.
#include <windows.h>

// {7C5B1A2E-3D4F-4B8A-9E61-2F0C8D5A7B13}
#define OPENCAM_CLSID_STR L"{7C5B1A2E-3D4F-4B8A-9E61-2F0C8D5A7B13}"
static const GUID CLSID_openCam = {0x7c5b1a2e, 0x3d4f, 0x4b8a, {0x9e, 0x61, 0x2f, 0x0c, 0x8d, 0x5a, 0x7b, 0x13}};

// Created by the DLL (LocalService may create Global\ objects), opened by the tray app.
#define OPENCAM_MAP_NAME L"Global\\openCamFrame"

constexpr UINT MAX_W = 3840, MAX_H = 2160;
enum : UINT { FLAG_MIRROR = 1, FLAG_ROT180 = 2 };

// One NV12 frame (stride == width). Seqlock: writer makes seq odd while writing.
struct SharedFrame {
    volatile LONG seq;
    UINT width, height, flags;
    volatile LONGLONG consumerTick; // GetTickCount64() of the last frame an app pulled
    volatile LONGLONG frameTick;    // GetTickCount64() of the last frame the tray app wrote
    BYTE data[MAX_W * MAX_H * 3 / 2];
};
