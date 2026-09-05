#include "stdafx.h"
#include "CGameloop.h"

HINSTANCE g_hInst;
LPCWSTR lpszClass = L"WindowClassName";
LPCWSTR lpszWindowName = L"Window Program gimal project";

LRESULT CALLBACK WndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam);

HDC hdc;
CGameloop loop;

SOCKET s_socket;
bool recvRunning = true;
HANDLE hRecvThread;

CRITICAL_SECTION UpdateCS;

const int clientWidth = 800;
const int clientHeight = 800;

std::string GetServerAddressFromConsole() {

    AllocConsole();                          // 콘솔 창 생성
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);  // 표준 출력 연결
    freopen_s(&fp, "CONIN$", "r", stdin);    // 표준 입력 연결

    std::string addr;
    std::cout << "서버 IP 입력: ";
    std::getline(std::cin, addr);

    HWND hwnd = GetConsoleWindow();
    FreeConsole();
    if (hwnd) PostMessage(hwnd, WM_CLOSE, 0, 0); // 안전하게 닫기

    return addr;
}
SOCKET ConnectToServer(const std::string& serverAddr, unsigned short port) {
    WSADATA WSAData;
    if (WSAStartup(MAKEWORD(2, 0), &WSAData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return INVALID_SOCKET;
    }

    SOCKET sock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "WSASocket failed." << std::endl;
        WSACleanup();
        return INVALID_SOCKET;
    }

    SOCKADDR_IN server_addr;
    ZeroMemory(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, serverAddr.c_str(), &server_addr.sin_addr) != 1) {
        std::cerr << "Invalid IP address." << std::endl;
        closesocket(sock);
        WSACleanup();
        return INVALID_SOCKET;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Connect failed." << std::endl;
        closesocket(sock);
        WSACleanup();
        return INVALID_SOCKET;
    }

    return sock;
}
DWORD WINAPI RecvThread(LPVOID lpParam) {
    // 넌블로킹 설정
    u_long mode = 1;
    ioctlsocket(s_socket, FIONBIO, &mode);

    while (recvRunning) {
        auto coord = std::make_unique<Coord_Packet>();
        WSABUF buf;
        buf.buf = (CHAR*)coord.get();
        buf.len = sizeof(Coord_Packet);
        DWORD received = 0;
        DWORD flags = 0;

        int result = WSARecv(s_socket, &buf, 1, &received, &flags, NULL, NULL);
        if (result == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                Sleep(1); // 데이터 없으면 살짝 쉬기
                continue;
            }
            else {
                OutputDebugString(L"WSARecv 실패\n");
                break;
            }
        }

        if (received == sizeof(Coord_Packet)) {
            EnterCriticalSection(&loop.UpdateCS);
            loop.RecvQueue.push(std::move(coord));
            LeaveCriticalSection(&loop.UpdateCS);
        }
    }

    return 0;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {

    HWND hWnd;
    MSG Message;
    WNDCLASSEX wcex;
    g_hInst = hInstance;

    std::string serverAddr = GetServerAddressFromConsole();
    s_socket = ConnectToServer(serverAddr, SERVER_PORT);

    if (s_socket == INVALID_SOCKET) {
        MessageBox(nullptr, L"서버 연결 실패!", L"Error", MB_OK);
        return 1;
    }

    hRecvThread = CreateThread(NULL, 0, RecvThread, NULL, 0, NULL);

    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = nullptr;
    wcex.lpszClassName = lpszClass;
    wcex.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassEx(&wcex)) {
        DWORD error = GetLastError();
        std::wstring errorMsg = L"RegisterClassEx failed with error code: " + std::to_wstring(error);
        MessageBox(nullptr, errorMsg.c_str(), L"Error", MB_OK);
        return 1;
    }

    RECT rect = { 0, 0, clientWidth, clientHeight };

    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    int windowWidth  = rect.right  - rect.left;
    int windowHeight = rect.bottom - rect.top;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - windowWidth) / 2;     
    int posY = (screenH - windowHeight) / 2;    

    hWnd = CreateWindowW(
        lpszClass,                   
        L"800x800 Client Area Window",
        WS_OVERLAPPEDWINDOW,          
        posX, posY,                   
        windowWidth, windowHeight,    
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) {
        MessageBox(nullptr, L"CreateWindow failed!", L"Error", MB_OK);
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    loop.Init(hWnd);

    ZeroMemory(&Message, sizeof(Message));
    while (Message.message != WM_QUIT) {
        if (PeekMessage(&Message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&Message);
            DispatchMessage(&Message);
        }
        else {   // 게임 업데이트 및 렌더링
            loop.Update();
            loop.Render();
        }
    }

    closesocket(s_socket);
    WSACleanup();
    
    return (int)Message.wParam;
}



// 윈도우 프로시저
LRESULT CALLBACK WndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam) {
    switch (iMessage) {
    case WM_CREATE:    
        break;
    case WM_DESTROY:
        recvRunning = false;
        WaitForSingleObject(hRecvThread, INFINITE);
        CloseHandle(hRecvThread);
        closesocket(s_socket);
        WSACleanup();
        PostQuitMessage(0);
        break;
    case WM_KEYDOWN:
        if ((lParam & (1 << 30)) == 0) {
            Input_Packet input{};
            switch (wParam) {
            case VK_DOWN:  input.inputType = KT::Down; break;
            case VK_UP:    input.inputType = KT::Up; break;
            case VK_RIGHT: input.inputType = KT::Right; break;
            case VK_LEFT:  input.inputType = KT::Left; break;
            default: return 0;
            }

            WSABUF buf;
            buf.buf = (CHAR*)&input;
            buf.len = sizeof(Input_Packet);
            DWORD sent;
            WSASend(s_socket, &buf, 1, &sent, 0, 0, 0);
        }
        break;
    default:
        return DefWindowProc(hWnd, iMessage, wParam, lParam);
    }
    
    return 0;
}

