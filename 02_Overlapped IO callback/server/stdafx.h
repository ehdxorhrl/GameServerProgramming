#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <iostream>
#include <WS2tcpip.h>
#include <string>
#include <unordered_map>

const short SERVER_PORT = 9000;
const int BUFSIZE = 256;

#pragma comment (lib, "WS2_32.LIB")

enum class KT : uint8_t { Up = VK_UP, Down = VK_DOWN, Left = VK_LEFT, Right = VK_RIGHT };

struct Input_Packet {
    KT inputType; // enum class KT : uint8_t
};

struct Coord_Packet {
    short x;
    short y;
};

void print_error_message(int s_err)
{
    WCHAR* lpMsgBuf;
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM,
        NULL, s_err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR)&lpMsgBuf, 0, NULL);
    std::wcout << lpMsgBuf << std::endl;
    while (true);   // µð¹ö±ë ¿ë
    LocalFree(lpMsgBuf);
}
