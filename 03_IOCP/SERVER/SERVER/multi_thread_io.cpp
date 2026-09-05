#include <iostream>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <vector>
#include <concurrent_unordered_map.h>
#include <thread>
#include <mutex>
#include "protocol.h"

#pragma comment(lib, "WS2_32.lib")
using namespace std;

enum S_STATE { ST_FREE, ST_ALLOC, ST_INGAME };

class SESSION {
public:
    S_STATE _state;
    int _id;
    SOCKET _socket;
    short x, y;
    char _name[NAME_SIZE];
    int _last_move_time;
    int _prev_remain;
    char _recv_buf[BUF_SIZE];

    SESSION() {
        _id = -1;
        _socket = 0;
        x = y = 0;
        _name[0] = 0;
        _state = ST_FREE;
        _last_move_time = 0;
        _prev_remain = 0;
    }

    void send_packet(void* packet) {
        if (_state != ST_INGAME) return; 
        if (_socket == INVALID_SOCKET) return;

        char* p = reinterpret_cast<char*>(packet);
        int total = 0;
        int size = p[0];
        while (total < size) {
            int sent = send(_socket, p + total, size - total, 0);
            if (sent == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    continue;
                }
                else {
                    return;
                }
            }
            total += sent;
        }
    }

    void send_login_info_packet();
    void send_move_packet(int c_id);
    void send_add_player_packet(int c_id);
    void send_remove_player_packet(int c_id);
};

concurrency::concurrent_unordered_map<int, shared_ptr<SESSION>> clients;

void SESSION::send_login_info_packet() {
    SC_LOGIN_INFO_PACKET p;
    p.id = _id;
    p.size = sizeof(SC_LOGIN_INFO_PACKET);
    p.type = SC_LOGIN_INFO;
    p.x = x;
    p.y = y;
    send_packet(&p);
}

void SESSION::send_move_packet(int c_id) {
    auto it = clients.find(c_id);
    if (it == clients.end()) return;
    SC_MOVE_PLAYER_PACKET p;
    p.id = c_id;
    p.size = sizeof(SC_MOVE_PLAYER_PACKET);
    p.type = SC_MOVE_PLAYER;
    p.x = it->second->x;
    p.y = it->second->y;
    p.move_time = it->second->_last_move_time;
    send_packet(&p);
}

void SESSION::send_add_player_packet(int c_id) {
    auto it = clients.find(c_id);
    if (it == clients.end()) return;
    SC_ADD_PLAYER_PACKET p;
    p.id = c_id;
    strcpy_s(p.name, it->second->_name);
    p.size = sizeof(SC_ADD_PLAYER_PACKET);
    p.type = SC_ADD_PLAYER;
    p.x = it->second->x;
    p.y = it->second->y;
    send_packet(&p);
}

void SESSION::send_remove_player_packet(int c_id) {
    SC_REMOVE_PLAYER_PACKET p;
    p.id = c_id;
    p.size = sizeof(SC_REMOVE_PLAYER_PACKET);
    p.type = SC_REMOVE_PLAYER;
    send_packet(&p);
}

int get_new_client_id() {
    for (int i = 0; i < MAX_USER; ++i) {
        if (clients.find(i) == clients.end()) return i;
    }
    return -1;
}

void process_packet(int c_id, char* packet) {
    auto it = clients.find(c_id);
    if (it == clients.end()) return;
    auto session = it->second;

    switch (packet[1]) {
    case CS_LOGIN: {
        CS_LOGIN_PACKET* p = reinterpret_cast<CS_LOGIN_PACKET*>(packet);
        strcpy_s(session->_name, p->name);
        session->send_login_info_packet();
        session->_state = ST_INGAME;
        for (auto& cl : clients) {
            auto other = cl.second;
            if (other->_state != ST_INGAME) continue;
            if (other->_id == c_id) continue;
            other->send_add_player_packet(c_id);
            session->send_add_player_packet(other->_id);
        }
        break;
    }
    case CS_MOVE: {
        CS_MOVE_PACKET* p = reinterpret_cast<CS_MOVE_PACKET*>(packet);
        session->_last_move_time = p->move_time;
        short x = session->x;
        short y = session->y;
        switch (p->direction) {
        case 0: if (y > 0) y--; break;
        case 1: if (y < W_HEIGHT - 1) y++; break;
        case 2: if (x > 0) x--; break;
        case 3: if (x < W_WIDTH - 1) x++; break;
        }
        session->x = x;
        session->y = y;
        for (auto& cl : clients) {
            auto other = cl.second;
            if (other->_state != ST_INGAME) continue;
            other->send_move_packet(c_id);
        }
        break;
    }
    }
}

void disconnect(int c_id) {
    auto it = clients.find(c_id);
    if (it == clients.end()) return;
    auto session = it->second;
    for (auto& cl : clients) {
        auto other = cl.second;
        if (other->_state != ST_INGAME) continue;
        if (other->_id == c_id) continue;
        other->send_remove_player_packet(c_id);
    }
    closesocket(session->_socket);
    session->_state = ST_FREE;
    cout << "Client " << c_id << " disconnected." << endl;
}

struct THREAD_INFO {
    thread th;
    bool alive = true;
};

mutex thread_info_mutex;
vector<shared_ptr<THREAD_INFO>> thread_infos;

void client_thread(shared_ptr<SESSION> session, shared_ptr<THREAD_INFO> info) {
    while (true) {
        int recv_bytes = recv(session->_socket, session->_recv_buf + session->_prev_remain, BUF_SIZE - session->_prev_remain, 0);
        if (recv_bytes > 0) {
            int total_bytes = recv_bytes + session->_prev_remain;
            char* p = session->_recv_buf;
            while (total_bytes > 0) {
                int packet_size = p[0];
                if (packet_size <= total_bytes) {
                    process_packet(session->_id, p);
                    p += packet_size;
                    total_bytes -= packet_size;
                }
                else break;
            }
            session->_prev_remain = total_bytes;
            if (total_bytes > 0)
                memcpy(session->_recv_buf, p, total_bytes);
        }
        else if (recv_bytes == 0 || (recv_bytes == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) {
            disconnect(session->_id);
            break;
        }
    }
    lock_guard<mutex> lock(thread_info_mutex);
    info->alive = false;
}

int main() {
    WSADATA WSAData;
    WSAStartup(MAKEWORD(2, 2), &WSAData);
    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT_NUM);
    server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
    bind(server, (sockaddr*)&server_addr, sizeof(server_addr));
    listen(server, SOMAXCONN);

    u_long mode = 1;
    ioctlsocket(server, FIONBIO, &mode);

    while (true) {
        sockaddr_in cl_addr;
        int addr_size = sizeof(cl_addr);
        SOCKET client = accept(server, (sockaddr*)&cl_addr, &addr_size);
        if (client == INVALID_SOCKET) {
            if (WSAGetLastError() != WSAEWOULDBLOCK)
                cout << "Accept error: " << WSAGetLastError() << endl;
        }
        else {
            int client_id = get_new_client_id();
            if (client_id != -1) {
                auto session = make_shared<SESSION>();
                session->_id = client_id;
                session->_socket = client;
                session->_state = ST_INGAME;
                clients[client_id] = session;

                ioctlsocket(client, FIONBIO, &mode);

                auto info = make_shared<THREAD_INFO>();
                thread t(client_thread, session, info);
                info->th = move(t);

                {
                    lock_guard<mutex> lock(thread_info_mutex);
                    thread_infos.emplace_back(info);
                }
            }
            else {
                cout << "Max user exceeded" << endl;
                closesocket(client);
            }
        }

        lock_guard<mutex> lock(thread_info_mutex);
        for (auto it = thread_infos.begin(); it != thread_infos.end();) {
            if (!(*it)->alive && (*it)->th.joinable()) {
                (*it)->th.join();
                it = thread_infos.erase(it);
            }
            else ++it;
        }
    }

    closesocket(server);
    WSACleanup();
}
