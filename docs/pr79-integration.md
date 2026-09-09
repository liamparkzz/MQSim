# PR79 변경 반영

사용자 요청에 따라 [공식 PR79](https://github.com/CMU-SAFARI/MQSim/pull/79)의 변경을 GitHub 백업 코드에 반영하고 옵시디언 변경 기록을 검증한다.

- PR head: `f42c4b82bff029638c15579029a4e31f22e31dba`
- PR 공통 조상: `7ec40e3b198e8bd117f992f051cd610e330e079e`
- 기존 원본 기준: `51f0f2d3fed92d88ef4a0fa61a38024b07bf9d16`
- 최초 원본은 Obsidian `Code/Baseline/`에 유지한다. `Code/Current/`는 이번 변경이 백업된 커밋에서 생성한다.

## 수정 이유 및 변경 설명

| 관련 파일/함수 | 수정 이유 | 변경 설명 |
|---|---|---|
| `SSD_Device.cpp` 생성자 | XML의 WL 설정이 GC/WL 객체로 전달되지 않음(001) | dynamic/static 활성화와 threshold를 seed 앞의 올바른 인자로 전달한다. |
| `Address_Mapping_Unit_Base.h`, `Address_Mapping_Unit_Page_Level.cpp`, Simple/Advanced 캐시 `Setup_triggers` | LPA barrier 뒤의 Host 요청에 완료 통지가 누락됨(003) | 캐시 완료 핸들러를 등록하고 barrier 해제 시 그 핸들러로 요청 완료를 전달한다. |
| `TSU_OutofOrder.cpp`, `TSU_Priority_OutOfOrder.cpp` | 매핑 쓰기가 선택되지 않고 큐에 남음(004) | 사용자/GC 쓰기와 함께 매핑 쓰기 큐도 선택하며, 다른 쓰기가 없어도 매핑 쓰기를 제출한다. |
| `Address_Mapping_Unit_Page_Level.cpp` | 매핑 읽기/쓰기 의존성과 사전 생성 페이지 상태 불일치 | 읽기 트랜잭션의 RelatedWrite를 연결하고, 사전 생성 페이지의 프로그램 카운터·메타데이터를 갱신한다. |
| `GC_and_WL_Unit_Base.cpp`, `GC_and_WL_Unit_Page_Level.cpp` | 페이지 이동과 erase 제출 시점의 불일치 | 이동할 페이지가 없거나 마지막 이동 쓰기가 끝났을 때 erase를 제출한다. |
| `Flash_Block_Manager.cpp`, `Flash_Block_Manager_Base.cpp` | 무효화 후 valid 카운트 누락과 64비트 이상 시프트 | valid 페이지 수를 감소시키고 bitmap 내부 비트 위치에 `page_id % 64`를 사용한다. |
| `Host_Interface_NVMe.cpp`, `Logical_Address_Partitioning_Unit.cpp` | 주소 계산 문제 | NVMe 상위 LBA를 31 대신 32비트 이동한다. 스트림 종료 주소 계산은 PR의 `start + count`를 반영한다. |
| `Host_System.cpp`, `SSD_Device.cpp`, `IO_Flow_Trace_Based.cpp`, `main.cpp`, 매핑/캐시/PHY/RandomGenerator 소멸자 | 동적 할당과 파일 자원 해제 누락 | 파생 타입 해제, 파일 닫기, XML 버퍼/시나리오/실행 설정·내부 배열·난수 생성기 등의 해제를 반영한다. |
| `SSD_Defs.h`, `Queue_Probe.cpp`, 캐시 이벤트 코드, `.gitignore` | 표현 명확화와 개발 파일 제외 | 매크로/지역변수 이름과 분기 표현을 정리하고 `.vscode`, `traces` 제외 규칙을 기존 규칙에 추가한다. 이미 추적 중인 trace는 유지한다. |

## 병합 시 보존 및 보정

- PR 이후 원본에 들어온 IO_Flow_Base 수정은 이미 동일하게 존재한다. Simple 캐시 빈 목록 해제, PHY의 미기록 페이지 메타데이터 보호, 불필요한 `lsa2` 제거를 유지했다.
- PR79의 `SSD_Device` 소멸자는 단일 `new ONFI_Channel_NVDDR2(...)` 객체에 `delete[]`를 적용한다. 그대로 적용하면 할당/해제 방식이 불일치하므로 파생 타입의 단일 `delete`로 보정했다.
- 따라서 현재 코드는 PR head의 전체 덮어쓰기와는 다르며, PR 변경을 현재 원본에 병합한 결과이다. 002나 003 잔류 문제의 추가 수정은 포함하지 않는다.

## 검증

- Windows 노트북, MSVC 14.51, C++14, `/Od`, 전체 시뮬레이터와 읽기 전용 관측 실행 파일 빌드 통과.
- SSD 256 MiB, CMT 48 B, 캐시/preconditioning 비활성, 순차 쓰기 128건, 요청 8 KiB, 간격 2 ms.
- `PRIORITY_OUT_OF_ORDER`/`OUT_OF_ORDER` × dynamic WL on/off × static WL on/off = 8조건. 각 조건을 일반 실행/관측 실행으로 비교하여 16프로세스 모두 정상 종료.
- 모든 조건: Host 128/128 완료, 매핑 쓰기 15건 완료 및 잔량 0, WL 활성화/임계값 500 객체 일치, 일반/관측 결과 XML 바이트 동일.
- 실행 전후 src 149개 파일 해시 동일. 결과: `C:/CODEX/MQsimExperiments/20260909-pr79-integration/summary.json`.

재현:

```powershell
& .\scripts\build-pr79-validation.ps1
python .\tests\run_pr79_validation.py
```

원본 비교 실험에서 002는 미해결, 003은 일부 완료 경로만 수정됐고 잔류/중복 잠금 문제가 남았다. 이번 소규모 검증은 그 긴 입력이나 다른 캐시 모드, 데이터 내용 정합성, 전체 메모리 누수 검증을 대신하지 않는다.

## 변경 기록 생성 방식

커밋 본문에 수정 이유·변경 설명·검증·관련 이슈를 작성한다. GitHub 백업 성공 후 기존 exporter를 실행하면 커밋 시각, 본문, 파일 목록, diff, GitHub 링크가 Obsidian `Code/History/commit-<해시>.md`에 자동 생성된다. 이 생성 노트를 직접 작성하지 않고 산출물을 검증한다.
