#include <iostream>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <thread>
#include <vector>
#include <mutex>
#include <concurrent_unordered_map.h>
#include "protocol.h"

#pragma comment(lib, "WS2_32.lib")
#pragma comment(lib, "MSWSock.lib")
using namespace std;

enum COMP_TYPE { OP_ACCEPT, OP_RECV, OP_SEND };
class OVER_EXP {
public:
    WSAOVERLAPPED _over;
    WSABUF _wsabuf;
    char _send_buf[BUF_SIZE];
    COMP_TYPE _comp_type;
    OVER_EXP()
    {
        _wsabuf.len = BUF_SIZE;
        _wsabuf.buf = _send_buf;
        _comp_type = OP_RECV;
        ZeroMemory(&_over, sizeof(_over));
    }
    OVER_EXP(char* packet)
    {
        _wsabuf.len = packet[0];
        _wsabuf.buf = _send_buf;
        ZeroMemory(&_over, sizeof(_over));
        _comp_type = OP_SEND;
        memcpy(_send_buf, packet, packet[0]);
    }
};

enum S_STATE { ST_FREE, ST_ALLOC, ST_INGAME };
class SESSION {
    OVER_EXP _recv_over;

public:
    mutex _s_lock;
    S_STATE _state;
    int _id;
    SOCKET _socket;
    short x, y;
    char _name[NAME_SIZE];
    int _prev_remain;
    int _last_move_time;
public:
    SESSION()
    {
        _id = -1;
        _socket = 0;
        x = y = 0;
        _name[0] = 0;
        _state = ST_FREE;
        _prev_remain = 0;
    }

    ~SESSION() {}

    void do_recv()
    {
        DWORD recv_flag = 0;
        memset(&_recv_over._over, 0, sizeof(_recv_over._over));
        _recv_over._wsabuf.len = BUF_SIZE - _prev_remain;
        _recv_over._wsabuf.buf = _recv_over._send_buf + _prev_remain;
        WSARecv(_socket, &_recv_over._wsabuf, 1, 0, &recv_flag,
            &_recv_over._over, 0);
    }

    void do_send(void* packet)
    {
        OVER_EXP* sdata = new OVER_EXP{ reinterpret_cast<char*>(packet) };
        WSASend(_socket, &sdata->_wsabuf, 1, 0, 0, &sdata->_over, 0);
    }
    void send_login_info_packet()
    {
        SC_LOGIN_INFO_PACKET p;
        p.id = _id;
        p.size = sizeof(SC_LOGIN_INFO_PACKET);
        p.type = SC_LOGIN_INFO;
        p.x = x;
        p.y = y;
        do_send(&p);
    }
    void send_move_packet(int c_id);
    void send_add_player_packet(int c_id);
    void send_remove_player_packet(int c_id)
    {
        SC_REMOVE_PLAYER_PACKET p;
        p.id = c_id;
        p.size = sizeof(p);
        p.type = SC_REMOVE_PLAYER;
        do_send(&p);
    }
};

concurrency::concurrent_unordered_map<long long, shared_ptr<SESSION>> clients;

SOCKET g_s_socket, g_c_socket;
OVER_EXP g_a_over;

void SESSION::send_move_packet(int c_id)
{
    auto it = clients.find(c_id);
    if (it == clients.end()) return;
    SC_MOVE_PLAYER_PACKET p;
    p.id = c_id;
    p.size = sizeof(SC_MOVE_PLAYER_PACKET);
    p.type = SC_MOVE_PLAYER;
    p.x = it->second->x;
    p.y = it->second->y;
    p.move_time = it->second->_last_move_time;
    do_send(&p);
}

void SESSION::send_add_player_packet(int c_id)
{
    auto it = clients.find(c_id);
    if (it == clients.end()) return;
    SC_ADD_PLAYER_PACKET add_packet;
    add_packet.id = c_id;
    strcpy_s(add_packet.name, it->second->_name);
    add_packet.size = sizeof(add_packet);
    add_packet.type = SC_ADD_PLAYER;
    add_packet.x = it->second->x;
    add_packet.y = it->second->y;
    do_send(&add_packet);
}

int get_new_client_id()
{
    for (int i = 0; i < MAX_USER; ++i) {
        auto it = clients.find(i);
        if (it == clients.end()) return i;
        if (it->second->_state == ST_FREE) return i;
    }
    return -1;
}

void process_packet(int c_id, char* packet)
{
    auto it = clients.find(c_id);
    if (it == clients.end()) return;

    switch (packet[1]) {
    case CS_LOGIN: {
        CS_LOGIN_PACKET* p = reinterpret_cast<CS_LOGIN_PACKET*>(packet);
        strcpy_s(it->second->_name, p->name);
        it->second->send_login_info_packet();
        {
            lock_guard<mutex> ll{ it->second->_s_lock };
            it->second->_state = ST_INGAME;
        }
        for (auto& pl : clients) {
            {
                lock_guard<mutex> ll(pl.second->_s_lock);
                if (ST_INGAME != pl.second->_state) continue;
            }
            if (pl.second->_id == c_id) continue;
            pl.second->send_add_player_packet(c_id);
            it->second->send_add_player_packet(pl.second->_id);
        }
        break;
    }
    case CS_MOVE: {
        CS_MOVE_PACKET* p = reinterpret_cast<CS_MOVE_PACKET*>(packet);
        it->second->_last_move_time = p->move_time;
        short x = it->second->x;
        short y = it->second->y;
        switch (p->direction) {
        case 0: if (y > 0) y--; break;
        case 1: if (y < W_HEIGHT - 1) y++; break;
        case 2: if (x > 0) x--; break;
        case 3: if (x < W_WIDTH - 1) x++; break;
        }
        it->second->x = x;
        it->second->y = y;

        for (auto& cl : clients) {
            if (cl.second->_state != ST_INGAME) continue;
            cl.second->send_move_packet(c_id);
        }
        break;
    }
    }
}

void disconnect(int c_id)
{
    auto it = clients.find(c_id);
    if (it == clients.end()) return;

    for (auto& pl : clients) {
        {
            lock_guard<mutex> ll(pl.second->_s_lock);
            if (ST_INGAME != pl.second->_state) continue;
        }
        if (pl.second->_id == c_id) continue;
        pl.second->send_remove_player_packet(c_id);
    }
    closesocket(it->second->_socket);

    lock_guard<mutex> ll(it->second->_s_lock);
    it->second->_state = ST_FREE;
}

void worker_thread(HANDLE h_iocp)
{
    while (true) {
        DWORD num_bytes;
        ULONG_PTR key;
        WSAOVERLAPPED* over = nullptr;
        BOOL ret = GetQueuedCompletionStatus(h_iocp, &num_bytes, &key, &over, INFINITE);
        OVER_EXP* ex_over = reinterpret_cast<OVER_EXP*>(over);
        if (FALSE == ret) {
            if (ex_over->_comp_type == OP_ACCEPT) cout << "Accept Error";
            else {
                cout << "GQCS Error on client[" << key << "]\n";
                disconnect(static_cast<int>(key));
                if (ex_over->_comp_type == OP_SEND) delete ex_over;
                continue;
            }
        }

        if ((0 == num_bytes) && ((ex_over->_comp_type == OP_RECV) || (ex_over->_comp_type == OP_SEND))) {
            disconnect(static_cast<int>(key));
            if (ex_over->_comp_type == OP_SEND) delete ex_over;
            continue;
        }

        switch (ex_over->_comp_type) {
        case OP_ACCEPT: {
            int client_id = get_new_client_id();
            if (client_id != -1) {
                clients[client_id] = make_shared<SESSION>();
                auto& client = clients[client_id];
                {
                    lock_guard<mutex> ll(client->_s_lock);
                    client->_state = ST_ALLOC;
                }
                client->x = 0;
                client->y = 0;
                client->_id = client_id;
                client->_name[0] = 0;
                client->_prev_remain = 0;
                client->_socket = g_c_socket;
                CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_c_socket),
                    h_iocp, client_id, 0);
                client->do_recv();
                g_c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
            }
            else {
                cout << "Max user exceeded.\n";
            }
            ZeroMemory(&g_a_over._over, sizeof(g_a_over._over));
            int addr_size = sizeof(SOCKADDR_IN);
            AcceptEx(g_s_socket, g_c_socket, g_a_over._send_buf, 0, addr_size + 16, addr_size + 16, 0, &g_a_over._over);
            break;
        }
        case OP_RECV: {
            auto it = clients.find(static_cast<int>(key));
            if (it == clients.end()) break;
            int remain_data = num_bytes + it->second->_prev_remain;
            char* p = ex_over->_send_buf;
            while (remain_data > 0) {
                int packet_size = p[0];
                if (packet_size <= remain_data) {
                    process_packet(static_cast<int>(key), p);
                    p = p + packet_size;
                    remain_data = remain_data - packet_size;
                }
                else break;
            }
            it->second->_prev_remain = remain_data;
            if (remain_data > 0) {
                memcpy(ex_over->_send_buf, p, remain_data);
            }
            it->second->do_recv();
            break;
        }
        case OP_SEND:
            delete ex_over;
            break;
        }
    }
}

int main()
{
    HANDLE h_iocp;

    WSADATA WSAData;
    WSAStartup(MAKEWORD(2, 2), &WSAData);
    g_s_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    SOCKADDR_IN server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT_NUM);
    server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
    bind(g_s_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
    listen(g_s_socket, SOMAXCONN);
    int addr_size = sizeof(server_addr);

    h_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
    CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_s_socket), h_iocp, 9999, 0);
    g_c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    g_a_over._comp_type = OP_ACCEPT;
    AcceptEx(g_s_socket, g_c_socket, g_a_over._send_buf, 0, addr_size + 16, addr_size + 16, 0, &g_a_over._over);

    vector <thread> worker_threads;
    int num_threads = std::thread::hardware_concurrency();
    for (int i = 0; i < num_threads; ++i)
        worker_threads.emplace_back(worker_thread, h_iocp);
    for (auto& th : worker_threads)
        th.join();
    closesocket(g_s_socket);
    WSACleanup();
}
