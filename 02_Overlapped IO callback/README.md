# 02. Overlapped I/O — Callback

Blocking 방식에서 벗어나 **WinSock Overlapped I/O와 Completion Routine**을 이용해 비동기 송수신 구조를 구현한 단계입니다.

각 클라이언트를 `SESSION` 객체로 관리하고, `WSARecv` / `WSASend` 완료 시 등록한 Callback에서 후속 처리를 수행하도록 구성했습니다.

## 핵심 구현

- `WSA_FLAG_OVERLAPPED` 소켓 사용
- `WSAOVERLAPPED` 기반 비동기 송수신
- `g_recv_callback`, `g_send_callback` Completion Routine
- 클라이언트별 `SESSION` 객체 관리
- `unordered_map`을 이용한 다중 클라이언트 관리
- 송신 완료 후 동적으로 할당한 Overlapped 객체 해제
- 좌표 변경 및 접속 종료 정보를 다른 클라이언트에 전달

## 처리 흐름

```text
WSARecv 요청
    │
    └─ I/O 진행 중
          │
          ▼
   g_recv_callback()
          │
          ├─ 패킷 처리
          ├─ 다른 클라이언트에 전송
          └─ 다음 WSARecv 등록
```

송신도 같은 방식으로 `WSASend` 완료 후 `g_send_callback()`에서 송신용 Overlapped 객체를 정리합니다.

## Session 관리

```text
g_users
 ├─ Client 0 → SESSION
 ├─ Client 1 → SESSION
 └─ Client N → SESSION
```

각 `SESSION`은 소켓, ID, 위치, 수신용 `WSAOVERLAPPED`와 버퍼를 보유합니다. 여러 Callback에서 전역 세션 컨테이너에 접근할 수 있어 `recursive_mutex`로 접근을 보호합니다.

## 주요 코드

| 파일 | 내용 |
| --- | --- |
| [`server/main.cpp`](./server/main.cpp) | Overlapped I/O, Session 및 Callback 기반 서버 |
| [`client/main.cpp`](./client/main.cpp) | 다중 객체 표시 및 네트워크 처리 |
| [`client/CGameloop.cpp`](./client/CGameloop.cpp) | 클라이언트 게임 루프 |

## 이전 단계와의 차이

| 01_TCP | 02_Overlapped I/O |
| --- | --- |
| I/O 완료까지 호출 흐름이 대기 | I/O 완료 후 Callback으로 처리 |
| 단일 연결 중심 구조 | Session 컨테이너로 여러 클라이언트 관리 |
| 동기식 송수신 | Overlapped 비동기 송수신 |

Callback 방식은 비동기 I/O를 경험하기에는 적합하지만, 접속자와 작업 종류가 증가할수록 완료 처리 흐름과 동기화 관리가 복잡해집니다.

다음 단계에서는 완료된 I/O를 하나의 Queue로 모으고 Worker Thread가 처리하는 **IOCP 구조**로 확장합니다.

> Next: [03. IOCP](../03_IOCP/)
