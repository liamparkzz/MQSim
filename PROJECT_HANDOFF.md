# Project Handoff

이 문서는 데스크탑과 노트북 사이의 작업 인계 기록이며, Obsidian 프로젝트 노트에 직접 접근할 수 없을 때 사용하는 저장소 내 보조 기록이다.

## 동기화 설정

- GitHub 저장소: `https://github.com/liamparkzz/MQSim.git`
- 기본 브랜치: `main`
- Obsidian Vault: `C:\Users\kevin\Obsidian\LiamObsidian`
- Obsidian 홈: `C:\Users\kevin\Obsidian\LiamObsidian\MQSim SSD 연구\MQSim Home.md`
- 실험 대시보드: `C:\Users\kevin\Obsidian\LiamObsidian\MQSim SSD 연구\Experiments\Experiment Dashboard.md`
- Obsidian 작업 정책: `C:\Users\kevin\Obsidian\LiamObsidian\MQSim SSD 연구\Operations\다중 기기 작업 정책.md`
- Obsidian Git 저장소: `https://github.com/liamparkzz/LiamObsidian.git` (`main`)
- 마지막 확인: 2026-09-03 (Asia/Seoul)

## 현재 상태

- 작업 기기: 노트북 환경
- Git 상태: `C:\CODEX\MQsim`을 MQSim Git 저장소 `main`으로 연결하고 5분 간격 자동 commit/rebase/push 예약 작업 검증 완료 (`b9b2c58`)
- Obsidian 상태: 자동 동기화 설정 기록을 GitHub `main`에 반영 완료 (`9a1f6e2`); 별도의 사용자 수정은 보존
- 실행 중인 시뮬레이션: 확인된 항목 없음
- 미동기화 항목: Obsidian `Core/MQSim 구조와 핵심 개념.md`의 기존 사용자 수정 1건 — 이번 작업에 포함하지 않고 보존

## 다음 작업

1. `C:\CODEX\MQsim`의 기준 commit `b9b2c58`에서 host write부터 GC 완료까지 호출 경로를 추적해 lock·상태 기반 배제·transaction dependency와 blocking 범위를 정리한다.
2. 현재 GC/WL 발동 조건, victim 선택, migration, erase와 스케줄링 정책을 코드 근거로 정리한다.
3. GC/WL 정책 변형을 설정으로 선택할 수 있게 설계한다.
4. SSD 스펙과 workload 시나리오 matrix를 고정하고 데스크탑에서 실험한다.
5. 성능, tail latency, GC 대기, WAF와 erase-count 편차를 함께 분석한다.

## 작업 기록

### 2026-09-03 정책 설정

- 작업 기기: 확인 불가
- Git 브랜치: 없음 또는 확인 불가
- Git 커밋: Obsidian `45c0047` (`Add multi-device workflow policy`)
- 작업 목적: 데스크탑과 노트북 간 연속 작업 정책 설정
- 완료한 내용: 다중 기기 작업, GitHub/Obsidian 동기화, 시뮬레이션 기록 및 충돌 처리 정책 작성
- 변경한 주요 파일: `AGENTS.md`, `PROJECT_HANDOFF.md`
- 실행/테스트 결과: 문서 생성 및 Obsidian GitHub `main` 원격 반영 확인
- 시뮬레이션 상태: 확인된 실행 없음
- 결과 저장 위치: `C:\CODEX\MQsim`
- 결정 사항: GitHub는 코드 기준, Obsidian은 작업 맥락 기준으로 사용하고 커밋 해시로 연결
- 미해결 문제: 현재 `C:\CODEX\MQsim`이 Git 저장소가 아니므로 MQSim GitHub 반영 불가
- 다음 작업: 실제 MQSim 저장소를 이 경로에 clone/연결한 뒤 `AGENTS.md`와 `PROJECT_HANDOFF.md` 최초 동기화
- 재개 명령 또는 참고사항: 작업 시작 시 `git status`와 Obsidian 최신 기록부터 확인

### 2026-09-03 다음 과제 등록

- 작업 기기: 노트북 환경
- Git 브랜치: Obsidian `main`; MQSim 분석 대상 commit 미확인
- Git 커밋: Obsidian `f459156` (`Plan MQSim GC and wear-leveling experiments`)
- 작업 목적: MQSim write 시 GC lock 처리와 wear-leveling/GC 정책을 규명하고 정책·SSD 스펙별 시나리오 실험 수행
- 완료한 내용: 과제 범위를 코드 분석, 정책 변형, 시나리오 실험과 결과 분석 단계로 구조화
- 변경한 주요 파일: `PROJECT_HANDOFF.md`, Obsidian 연구 허브·실험 계획·신규 과제 노트
- 실행/테스트 결과: 아직 코드 분석 및 시뮬레이션을 시작하지 않음
- 시뮬레이션 상태: 예정
- 결과 저장 위치: 추후 Run ID별 경로 확정 필요
- 결정 사항: lock 이름 검색에 한정하지 않고 상태 기반 배제와 event-driven 직렬화까지 분석하며, 실험에서는 한 번에 한 변수만 변경
- 미해결 문제: 실제 MQSim 저장소 경로와 기준 commit 확인 필요
- 다음 작업: write/GC/WL 관련 파일·클래스·함수 목록 생성
- 재개 명령 또는 참고사항: 저장소 확인 후 `git status`, `git rev-parse HEAD`, 관련 심볼 검색부터 시작

### 2026-09-03 Obsidian 구조 개편

- 작업 기기: 노트북 환경
- Git 브랜치: Obsidian `main`
- Git 커밋: Obsidian `2a94c71` (`vault backup: 2026-09-03 12:59:29`)
- 작업 목적: 날짜·번호 나열식 노트를 MQSim 기능 뼈대와 연구 흐름이 보이는 혼합형 구조로 개편
- 완료한 내용: `MQSim Home`, 영역별 Map, 실험 Dashboard, Active/Completed 과제 폴더와 실험 Template 구성
- 변경한 주요 파일: Obsidian `MQSim SSD 연구` 아래 기존 노트 10개 이동·이름 변경 및 지도/템플릿 10개 추가
- 실행/테스트 결과: 기존 번호 위키링크 0개, 중복 노트명 0개, MQSim 영역 내 해석되지 않는 위키링크 0개
- 시뮬레이션 상태: 새로 시작한 실행 없음
- 결과 저장 위치: `C:\Users\kevin\Obsidian\LiamObsidian\MQSim SSD 연구`
- 결정 사항: 영어 카테고리 폴더와 한글 중심 노트명을 사용하고 새 과제는 `Experiments/Active/<주제명>`으로 관리
- 미해결 문제: 사용자가 실제 구조를 사용해 본 뒤 폴더명, 언어와 세분화 정도에 대한 취향 피드백 필요
- 다음 작업: `MQSim Home`과 `Experiment Dashboard`를 검토하고 선호에 따라 2차 조정
- 재개 명령 또는 참고사항: 구조 조정 시 파일 삭제 대신 Git 이동을 사용하고 전체 위키링크를 다시 검사

### 2026-09-03 MQSim 작업 경로 및 자동 동기화 설정

- 작업 기기: 노트북 환경
- Git 브랜치: MQSim `main`
- Git 커밋: `b9b2c58` (`auto-sync: 2026-09-03 13:46:05 [LAPTOP-ASUSZENB]`)
- 작업 목적: MQSim 코드를 `C:\CODEX\MQsim`에서 관리하고 변경사항을 GitHub에 자동 반영
- 완료한 내용: 원격 `main` 연결, 기존 정책 문서 보존·커밋, 5분 간격 Windows 예약 작업 `MQSim Git Auto Sync` 등록, 자동 commit/fetch/rebase/push 및 충돌·비밀정보·50MB 초과 파일 보호 구성
- 변경한 주요 파일: `.gitignore`, `AGENTS.md`, `AUTO_SYNC.md`, `PROJECT_HANDOFF.md`, `scripts/install-auto-sync-task.ps1`, `scripts/mqsim-auto-sync.ps1`
- 실행/테스트 결과: PowerShell 구문 검사와 `git diff --check` 통과; 예약 작업 `LastTaskResult=0`; 로컬 HEAD와 `origin/main`이 `b9b2c58`로 일치
- 시뮬레이션 상태: 새로 시작한 실행 없음
- 결과 저장 위치: 저장소 `C:\CODEX\MQsim`; 자동 동기화 로그 `C:\CODEX\MQsim\.git\auto-sync.log`
- 결정 사항: `main`만 5분마다 자동 동기화하고 충돌 시 rebase를 중단해 수동 확인; 바탕화면 복제본은 사용자가 삭제함
- 미해결 문제: 예약 작업은 현재 Codex 번들 Git 절대 경로를 사용하므로 런타임 위치가 바뀌면 설치 스크립트를 다시 실행해야 함; Obsidian `Core/MQSim 구조와 핵심 개념.md`의 기존 사용자 수정은 별도 확인 필요
- 다음 작업: `C:\CODEX\MQsim`에서 GC/WL 호출 경로와 정책 코드 분석 시작
- 재개 명령 또는 참고사항: `git -C C:\CODEX\MQsim status --short --branch`; `Get-ScheduledTaskInfo -TaskName 'MQSim Git Auto Sync'`; `Get-Content C:\CODEX\MQsim\.git\auto-sync.log -Tail 20`

### 2026-09-03 13:56 바탕화면 복제본 정리

- 작업 기기: 노트북 환경
- Git 브랜치: MQSim `main`
- Git 커밋: 작업 전 기준 `25cf520`; 이 기록은 자동 동기화 커밋으로 반영
- 작업 목적: MQSim 작업 경로를 `C:\CODEX\MQsim` 하나로 정리
- 완료한 내용: 사용자가 `C:\Users\kevin\Desktop\MQSim` 복제본을 삭제했고 해당 경로가 존재하지 않음을 확인
- 변경한 주요 파일: `PROJECT_HANDOFF.md`, Obsidian `Operations/다중 기기 작업 정책.md`
- 실행/테스트 결과: `Test-Path C:\Users\kevin\Desktop\MQSim` 결과 `False`; 기준 저장소 상태 `main...origin/main`
- 시뮬레이션 상태: 새로 시작한 실행 없음
- 결과 저장 위치: 공식 MQSim 작업 경로 `C:\CODEX\MQsim`
- 결정 사항: 앞으로 노트북의 MQSim 코드 작업은 `C:\CODEX\MQsim`에서만 수행
- 미해결 문제: Obsidian `Core/MQSim 구조와 핵심 개념.md`의 기존 사용자 수정은 별도 확인 필요
- 다음 작업: `C:\CODEX\MQsim`에서 GC/WL 호출 경로와 정책 코드 분석 시작
- 재개 명령 또는 참고사항: `git -C C:\CODEX\MQsim status --short --branch`

### 2026-09-03 15:49 자동 동기화 창 숨김 처리

- 작업 기기: 노트북 환경
- Git 브랜치: MQSim `main`
- Git 커밋: `3eb4006` (`auto-sync: 2026-09-03 15:48:43 [LAPTOP-ASUSZENB]`)
- 작업 목적: 5분마다 나타나던 PowerShell 터미널 창을 없애고 숨김 자동 push 유지
- 완료한 내용: `wscript.exe` 숨김 실행기 추가, 예약 작업 재활성화, 인증 팝업 금지, 원격과 같을 때 불필요한 push 생략, HTTP 무진행 30초 및 전체 실행 4분 제한 적용
- 변경한 주요 파일: `AUTO_SYNC.md`, `scripts/run-auto-sync-hidden.vbs`, `scripts/install-auto-sync-task.ps1`, `scripts/mqsim-auto-sync.ps1`, `PROJECT_HANDOFF.md`
- 실행/테스트 결과: 숨김 실행기 자동 commit/push 종료 코드 `0`; 예약 작업 `Ready`·`Enabled=True`·실행 파일 `wscript.exe`·`LastTaskResult=0`; 잔존 동기화 프로세스 없음
- 시뮬레이션 상태: 새로 시작한 실행 없음
- 결과 저장 위치: 저장소 `C:\CODEX\MQsim`; 로그 `C:\CODEX\MQsim\.git\auto-sync.log`
- 결정 사항: 5분 간격 자동 동기화는 창 없는 WScript 래퍼로 실행하고 실패·충돌은 로그로 확인
- 미해결 문제: Codex 번들 Git 경로가 바뀌면 설치 스크립트 재실행 필요; Obsidian `Core/MQSim 구조와 핵심 개념.md`의 기존 사용자 수정은 별도 확인 필요
- 다음 작업: `C:\CODEX\MQsim`에서 GC/WL 호출 경로와 정책 코드 분석 시작
- 재개 명령 또는 참고사항: `Get-ScheduledTaskInfo -TaskName 'MQSim Git Auto Sync'`; `Get-Content C:\CODEX\MQsim\.git\auto-sync.log -Tail 20`


### 2026-09-03 15:52 GC lock 및 User I/O 경합 경로 분석

- 작업 기기: 노트북 환경
- Git 브랜치: MQSim 원격 `main`; 로컬 상태는 Codex 실행 계층 `setup refresh` 오류로 확인 불가
- Git 커밋: 분석 기준 MQSim `11aa98afde2a070155ca97c4a82b4482110f1b72`; 인계 기록 최초 반영 `2a8303e2e85182ff45902624859b25f922913605`; Obsidian 분석 노트 최초 반영 `0e8bb48d1c50e1af501afb2491db9b9a9faca16a`
- 작업 목적: GC 실행 중 victim block 또는 같은 flash chip으로 들어오는 User Read/Write의 차단·대기·스케줄링 구조 규명
- 완료한 내용: GC victim 예약, `Has_ongoing_gc_wl`, 진행 중 User I/O 카운터, LPA/MVPN barrier, TSU User/GC 분리 큐, erase/program suspend, erase dependency 흐름을 추적하고 Obsidian에 정리
- 변경한 주요 파일: 코드 변경 없음; `PROJECT_HANDOFF.md`, Obsidian `GC lock 코드 분석.md`, 연구 계획, Experiment Dashboard
- 실행/테스트 결과: 비공개 GitHub `main` 정적 분석 완료; 로컬 명령 실행 및 시뮬레이션은 `helper_unknown_error: setup refresh had errors`로 수행하지 못함
- 시뮬레이션 상태: 새로 시작한 실행 없음; 기존 실행 여부는 로컬에서 독립 확인하지 못함
- 결과 저장 위치: `PROJECT_HANDOFF.md`; Obsidian `Experiments/Active/GC Lock과 Wear Leveling/GC lock 코드 분석.md`
- 결정 사항: GC lock은 flash 전체 mutex가 아니라 victim block의 valid LPA/MVPN barrier와 block별 GC 상태·User I/O 카운터의 조합이며, 다른 block I/O는 TSU 우선순위와 suspend 정책에 따라 계속 처리됨
- 미해결 문제: (1) 기존 User I/O 완료 후 지연 시작되는 GC 경로에서 `gc_wl_erase_tr`을 TSU에 제출하지 않아 erase 및 GC 상태 해제가 누락될 가능성, (2) LPA barrier 해제 시 대기 User I/O를 Address Mapping handler에 직접 전달한 뒤 삭제하지만 해당 handler가 non-MAPPING source를 즉시 반환하여 요청이 replay·완료 처리되지 않을 가능성, (3) 로컬 HEAD와 미커밋 변경 확인 필요
- 다음 작업: 두 경합 경로의 최소 재현 trace와 회귀 테스트를 만든 뒤 erase transaction 제출 및 barrier 대기 User I/O replay/completion 경로 수정
- 재개 명령 또는 참고사항: `git -C C:\CODEX\MQsim status --short --branch`; `rg -n "Set_barrier_for_accessing_physical_block|Remove_barrier_for_accessing_lpa|Can_execute_gc_wl|gc_wl_erase_tr" src/ssd`


### 2026-09-03 16:21 Obsidian 자동 pull 미실행 진단 및 수동 복구

- 작업 기기: 노트북 환경
- Git 브랜치: MQSim `main`; Obsidian `main`
- Git 커밋: 진단 전 MQSim `377b892`; Obsidian 원격 `f2ab2f0`; 이 기록 커밋은 반영 후 확인
- 작업 목적: Obsidian Git의 5분 auto-pull이 원격 GC lock 분석 노트를 로컬 Vault에 반영하지 않은 원인 확인
- 완료한 내용: MQSim 예약 작업은 MQSim 저장소만 대상으로 정상 실행됨을 확인. Obsidian Git 설정은 auto-pull 5분이지만 Custom Git binary path가 비어 있고 사용자·시스템 PATH에도 Git이 없어 일반 실행된 Obsidian에서 Git 자동 루틴이 동작하지 않은 것으로 진단. 원격 변경과 로컬 수정이 겹치지 않음을 확인하고 Obsidian 저장소를 `f2ab2f0`까지 fast-forward
- 변경한 주요 파일: 코드 변경 없음; `PROJECT_HANDOFF.md`, Obsidian `Operations/다중 기기 작업 정책.md`
- 실행/테스트 결과: Obsidian 로컬 HEAD `f2ab2f0`, `main...origin/main`; `GC lock 코드 분석.md` 존재 확인; 기존 로컬 수정 2개 보존
- 시뮬레이션 상태: 새로 시작한 실행 없음
- 결과 저장 위치: `C:\Users\kevin\Obsidian\LiamObsidian\MQSim SSD 연구\Experiments\Active\GC Lock과 Wear Leveling\GC lock 코드 분석.md`
- 결정 사항: 현재 5분 Windows 예약 작업은 MQSim 전용이며 Obsidian 동기화는 플러그인의 Git 실행 경로가 정상일 때만 동작
- 미해결 문제: Obsidian Git Custom Git binary path 설정 및 재시작 후 실제 5분 auto-pull 재검증 필요; Codex 번들 Git 경로 변경 가능성
- 다음 작업: Obsidian Git 경로 설정 후 검증용 원격 변경을 만들고 5분 이내 자동 pull 여부 확인
- 재개 명령 또는 참고사항: Obsidian 설정 → Community plugins → Git → Custom Git binary path
