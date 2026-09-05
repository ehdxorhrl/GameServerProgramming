#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <iostream>
#include <windows.h>
#include <WS2tcpip.h>
#include <atlImage.h>
#include <string>
#include <queue>

constexpr short SERVER_PORT = 9000;

#pragma comment (lib, "WS2_32.LIB")

enum class KT { Up = VK_UP, Down = VK_DOWN, Left = VK_LEFT, Right = VK_RIGHT };

struct Input_Packet {
    KT inputType; // enum class KT : uint8_t
};

struct Coord_Packet {
    short x;
    short y;
};