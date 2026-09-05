# 07. Database — ODBC Persistence

IOCP + NPC + Lua 서버에 **ODBC 기반 DB 연동**을 추가하여 플레이어 정보를 서버 실행 메모리 밖에 저장할 수 있도록 확장한 단계입니다.

로그인 시 DB에서 사용자 존재 여부와 마지막 위치를 조회하고, 연결 종료 시 현재 위치를 다시 DB에 저장합니다.

## 로그인 흐름

```text
Client Login
    │
    │ User ID
    ▼
check_user_exists()
    │
    ├─ 존재하지 않음 → SC_LOGIN_FAIL
    │
    └─ 존재함
         │
         ▼
get_user_position()
         │
         ▼
Session x, y 복원
         │
         ▼
SC_LOGIN_INFO
```

`CS_LOGIN_PACKET`으로 전달된 사용자 ID를 DB에서 확인한 뒤 유효한 사용자만 게임에 진입시킵니다.

## 접속 종료 흐름

```text
Client Disconnect
      │
      ▼
현재 Session의
uid / x / y
      │
      ▼
store_user_position()
      │
      ▼
Database
```

연결이 종료되기 전에 현재 좌표를 저장하여 다음 로그인 시 마지막 위치에서 이어서 시작할 수 있도록 했습니다.

## ODBC 처리

C++에서 ODBC API를 이용해 환경, 연결, Statement Handle을 생성하고 Prepared Statement와 Parameter Binding을 사용합니다.

### 사용자 존재 확인

```sql
SELECT dbo.user_exists(?)
```

### 사용자 위치 조회

```sql
SELECT x, y FROM get_user_position(?)
```

### 사용자 위치 저장

```sql
EXEC store_user_pos ?, ?, ?
```

입력 값은 `SQLBindParameter()`로 바인딩하고 조회 결과는 `SQLBindCol()` / `SQLFetch()`로 가져옵니다.

## 기존 서버와 통합

DB 기능은 별도의 예제 서버가 아니라 이전 단계의 기능 위에 추가되어 있습니다.

```text
TCP
 ↓
IOCP Multi-thread
 ↓
AOI / Spatial Map
 ↓
NPC / Timer
 ↓
Lua AI
 ↓
ODBC Database
```

따라서 `AI_Lua.cpp`에는 네트워크 처리, NPC AI, Lua Script와 함께 로그인/로그아웃 DB 처리가 포함되어 있습니다.

## 주요 파일

| 파일 | 내용 |
| --- | --- |
| [`AI_Lua.cpp`](./SERVER/SERVER/AI_Lua.cpp) | IOCP + Lua + ODBC 서버 |
| [`protocol.h`](./SERVER/SERVER/protocol.h) | DB 로그인 정보가 포함된 패킷 |
| [`npc.lua`](./SERVER/SERVER/npc.lua) | NPC AI Script |
| [`ODBC.txt`](./ODBC.txt) | ODBC 관련 설정 메모 |
| [`2025_Game_2020182029.bak`](./2025_Game_2020182029.bak) | 실습 DB Backup |

## 현재 구현에서의 확장 포인트

현재 DB 함수는 요청 시 ODBC Handle을 생성하고 DB에 연결한 뒤 작업 후 연결을 해제하는 단순한 학습용 구조입니다.

실제 대규모 서버로 확장한다면 다음과 같은 구조를 추가로 고려할 수 있습니다.

- DB Connection Pool
- DB 작업 전용 Worker / Queue
- 네트워크 Worker와 DB I/O 분리
- 실패 재시도 및 Transaction 처리
- DB 작업의 비동기화

이 단계까지를 통해 **네트워크 연결 → 비동기 I/O → 동시성 → 관심 영역 최적화 → 서버 AI → Script → 영속 데이터**로 이어지는 게임 서버의 기능 확장 과정을 구현했습니다.
