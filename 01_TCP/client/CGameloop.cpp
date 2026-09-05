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
    std::queue<std::unique_ptr<Coord_Packet>> TempQueue;

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
        auto* objcoord = static_cast<Coord_Packet*>(packet.get());

        x = objcoord->x;
        y = objcoord->y;
    }
}


void CGameloop::Render() {
    HDC mdc;
    HBITMAP hBitmap;
    RECT rt;
    GetClientRect(hwnd, &rt);
    hdc = GetDC(hwnd);
    mdc = CreateCompatibleDC(hdc); // 메모리 DC 생성
    hBitmap = CreateCompatibleBitmap(hdc, rt.right, rt.bottom); // 비트맵 생성
    SelectObject(mdc, hBitmap); // 비트맵을 메모리 DC에 선택

    Rectangle(mdc, 0, 0, rt.right, rt.bottom);
    imgboard.Draw(mdc, 0, 0, 800, 800, 0, 0, 800, 800);
    imgBlackKnight.Draw(mdc, x, y, 100, 100, 0, 0, 512, 512);

    // 백 버퍼의 내용을 화면에 복사
    BitBlt(hdc, 0, 0, rt.right, rt.bottom, mdc, 0, 0, SRCCOPY);

    // GDI 객체 원상복귀 및 자원 해제
    DeleteObject(hBitmap); // 비트맵 해제
    DeleteDC(mdc); // 메모리 DC 해제
    ReleaseDC(hwnd, hdc);
}

void CGameloop::SetServerSocket(SOCKET socket) {
    serverSocket = socket;
}