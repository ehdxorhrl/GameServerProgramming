#include "stdafx.h"
#include "CGameloop.h"

CGameloop::CGameloop() {
    InitializeCriticalSection(&UpdateCS);
}

CGameloop::~CGameloop() {
    DeleteCriticalSection(&UpdateCS);
}

void CGameloop::Init(HWND hWnd) {
    hwnd = hWnd;
    imgboard.Load(L"체스판.jpg");
    imgBlackKnight.Load(L"체스말.jpg");
}

void CGameloop::Update() {
    std::queue<std::unique_ptr<ServerPacket>> TempQueue;

    EnterCriticalSection(&UpdateCS);
    while (!RecvQueue.empty()) {
        TempQueue.push(std::move(RecvQueue.front()));
        RecvQueue.pop();
    }
    LeaveCriticalSection(&UpdateCS);

    if (TempQueue.empty()) {
        return;
    }

    while (!TempQueue.empty()) {
        auto packet = std::move(TempQueue.front());
        TempQueue.pop();
        auto* serverPacket = static_cast<ServerPacket*>(packet.get());

        if (serverPacket->type == PacketType::Coord) {
            horses[serverPacket->id] = { serverPacket->x, serverPacket->y };
        }
        else if (serverPacket->type == PacketType::Disconnect) {
            horses.erase(serverPacket->id);
        }
    }
}

void CGameloop::Render() {
    HDC mdc;
    HBITMAP hBitmap;
    RECT rt;
    GetClientRect(hwnd, &rt);
    hdc = GetDC(hwnd);
    mdc = CreateCompatibleDC(hdc);
    hBitmap = CreateCompatibleBitmap(hdc, rt.right, rt.bottom);
    SelectObject(mdc, hBitmap);

    Rectangle(mdc, 0, 0, rt.right, rt.bottom);
    imgboard.Draw(mdc, 0, 0, 800, 800, 0, 0, 800, 800);

    // 모든 말의 위치를 렌더링
    for (const auto& horse : horses) {
        short x = horse.second.first;
        short y = horse.second.second;
        imgBlackKnight.Draw(mdc, x, y, 100, 100, 0, 0, 512, 512);
    }

    BitBlt(hdc, 0, 0, rt.right, rt.bottom, mdc, 0, 0, SRCCOPY);

    DeleteObject(hBitmap);
    DeleteDC(mdc);
    ReleaseDC(hwnd, hdc);
}

void CGameloop::SetServerSocket(SOCKET socket) {
    serverSocket = socket;
}