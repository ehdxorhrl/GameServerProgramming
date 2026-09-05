#include "stdafx.h"

short x, y;

int main()
{

	WSADATA WSAdata;
	WSAStartup(MAKEWORD(2, 0), &WSAdata);

	SOCKET s_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, 0);

	SOCKADDR_IN server_addr;
	ZeroMemory(&server_addr, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(SERVER_PORT);
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind(s_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
	listen(s_socket, SOMAXCONN);

	INT addr_size = sizeof(server_addr);
	SOCKET c_socket = WSAAccept(s_socket, reinterpret_cast<sockaddr*>(&server_addr), &addr_size, 0, 0);

	while(true) {
		Input_Packet input_pk;
		WSABUF mybuf;
		mybuf.buf = (CHAR*)&input_pk; mybuf.len = sizeof(Input_Packet);
		DWORD recv_byte;
		DWORD recv_flag = 0;
		auto ref = WSARecv(c_socket, &mybuf, 1, &recv_byte, &recv_flag, 0, 0);

		if ((int)input_pk.inputType == 0) {
			continue;
		}

		BYTE* raw = (BYTE*)&input_pk;
		for (int i = 0; i < sizeof(Input_Packet); ++i)
			std::cout << "byte[" << i << "] = " << (int)raw[i] << std::endl;


		switch (input_pk.inputType)
		{
		case KT::Down:
			if (y < 700)
				y = y + 100;
			break;
		case KT::Up:
			if (y >= 100)
				y = y - 100;
			break;
		case KT::Right:
			if (x < 700)
				x = x + 100;
			break;
		case KT::Left:
			if (x >= 100)
				x = x - 100;
			break;
		default:
			break;
		}

		std::cout << "xÀÇ ÁÂÇ¥: " << x << std::endl;
		std::cout << "yÀÇ ÁÂÇ¥: " << y << std::endl;
		
		Coord_Packet coord_pk;
		coord_pk.x = x;
		coord_pk.y = y;

		DWORD sent_byte;
		mybuf.buf = (CHAR*)&coord_pk;
		mybuf.len = sizeof(Coord_Packet);
		if (WSASend(c_socket, &mybuf, 1, &sent_byte, 0, 0, 0) == SOCKET_ERROR) {
			std::cerr << "WSASend error: " << WSAGetLastError() << std::endl;
			break;
		}
	}

	WSACleanup();
}