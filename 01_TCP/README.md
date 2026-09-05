# 01. TCP — Blocking Socket

게임 서버 통신의 가장 기본이 되는 **TCP 연결과 Blocking I/O 흐름**을 구현한 단계입니다.

서버가 클라이언트의 입력 패킷을 수신하고, 서버에서 좌표를 갱신한 뒤 결과 좌표를 다시 클라이언트에 전달하는 간단한 구조로 TCP 소켓의 기본 동작을 확인했습니다.

## 핵심 구현

- WinSock 초기화 및 TCP 소켓 생성
- `bind` → `listen` → `accept`를 통한 연결 수립
- `WSARecv` / `WSASend`를 이용한 동기식 송수신
- 클라이언트 입력을 서버에서 처리한 뒤 좌표를 반환
- 구조체 기반 바이너리 패킷 송수신

## 처리 흐름

```text
Client
  │
  │ Input_Packet
  ▼
Server
  │
  ├─ 입력 방향 확인
  ├─ 서버 좌표 갱신
  │
  │ Coord_Packet
  ▼
Client
```

서버는 클라이언트가 직접 좌표를 결정하도록 두지 않고, 전달받은 방향 입력을 기준으로 서버 측의 `x`, `y` 값을 변경한 뒤 결과를 반환합니다.

## 주요 코드

| 파일 | 내용 |
| --- | --- |
| [`server/main.cpp`](./server/main.cpp) | TCP 서버 생성, 입력 수신, 좌표 갱신 및 결과 전송 |
| [`client/main.cpp`](./client/main.cpp) | 입력 패킷 전송 및 서버 좌표 수신 |
| [`client/CGameloop.cpp`](./client/CGameloop.cpp) | 클라이언트 화면 및 게임 루프 |

## 이 단계에서 확인한 점

Blocking 방식은 코드 흐름이 단순해 TCP 통신 과정을 이해하기 쉽지만, `recv`가 완료될 때까지 실행 흐름이 대기하므로 많은 클라이언트를 동시에 처리하는 서버 구조로 확장하기 어렵습니다.

다음 단계에서는 소켓 I/O가 끝날 때까지 스레드가 기다리지 않도록 **Overlapped I/O와 Completion Routine**을 적용합니다.

> Next: [02. Overlapped I/O Callback](../02_Overlapped%20IO%20callback/)
