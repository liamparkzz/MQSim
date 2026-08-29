# gun_swans MSRC_2 full-trace experiment

이 폴더는 `Trace/MSRC_2`를 `gun_swans`로 재생한 실험 묶음이다. 2026-08-29 사용자 요청으로 전체 실행을 보류했으며, 현재 정상 완료된 19개 트레이스의 원본 XML과 통합 Excel을 보존한다. 미완료 트레이스는 manifest에 실패/보류 상태와 함께 남긴다.

## 결과 위치

- `results/raw_xml/`: 트레이스별 MQSim 원본 XML
- `results/excel/gun_swans_MSRC2_paper_epoch_mu_results.xlsx`: 통합 결과표
- `manifest/trace_manifest.csv`: 트레이스·소스 리비전·실행 파일 SHA-256·실행 상태
- `source/`: 실행에 사용한 소스 스냅샷
- `bin/`: 버전별 실행 파일
- `inputs/`: SSD 설정과 트레이스별 workload XML
- `logs/`: 변환기와 MQSim 실행 로그
- `tools/`: 트레이스 변환, 배치 실행, Excel 생성·검증 도구

## 주요 설정

- SSD 4개, stripe unit 512 LBA
- SWANS zone 32,768 LBA = 16 MiB
- epoch 40,000,000,000 ns = 40 s
- μ 경계값: precautionary 5 pp, critical 15 pp
- 동시 migration 최대 1개
- OP 20%, 초기 점유율 70%
- device cache OFF, buffered write completion `DEFERRED`
- 원본 시간 1×, 전체 요청 100%, relay 1, `GLOBAL_LBA`
- block P/E limit 10,000; OP 고갈 시 즉시 종료 및 EOL 스냅샷 기록

## 소스 리비전

- `gun_swans_v1_epochlog`: 9개 EOL 필드와 epoch별 μ/상태/쓰기량 로그를 추가한 최초 실험 버전
- `gun_swans_v1_1_gcwl_fix`: 정적 wear-leveling이 잘못된 물리 주소를 잠그던 호출 수정
- `gun_swans_v1_2_gcwl_barrier_guard`: GC/WL 후보 블록의 LPA/MVPN 중복 장벽 방지 추가
- `gun_swans_v1_3_gc_candidate_fallback`: 잠긴 RGA 후보 대신 같은 plane의 안전한 대체 후보를 선택하는 교착 방지 초안. 빌드만 확인했으며 결과 XML 생성에는 아직 사용하지 않음

각 트레이스가 실제로 사용한 리비전과 바이너리 SHA-256은 Excel의 `Summary`/`Trace Manifest` 시트와 `manifest/trace_manifest.csv`에서 확인한다.

## Excel 시트

- `Summary`: 트레이스별 핵심 결과와 소스 리비전
- `Parameters`: 설정과 단위
- `EOL Audit`: 요청된 9개 필드의 과거/현재 포함 여부와 정의
- `Epoch Mu`: 모든 트레이스의 매 epoch μ, SWANS 상태, hot/cold SSD, 누적 쓰기 비율
- `Per SSD`: SSD별 host/migration 쓰기, OP, bad block, erase 통계
- `Migration Log`: 수행된 migration 세부 기록
- `Raw Summary`, `Trace Manifest`, `Source Files`: 원자료와 재현성 정보
