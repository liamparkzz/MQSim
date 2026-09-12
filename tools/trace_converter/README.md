# MQSim 공용 트레이스 변환기

버전 **1.0.0**. Alibaba는 제외한다. 여러 사람이 동일한 규칙으로 원본 MQSim 입력을 만드는 독립 프로그램이다. 시뮬레이터 `src/`를 수정하거나 시뮬레이션을 실행하지 않는다.

실제 파일 6개의 전체 변환·검증 결과와 256GB 주소 범위 확인 사항은 [VALIDATION.md](VALIDATION.md)에 있다.

## 실행

Windows에서는 **`Start-Converter.cmd`를 더블클릭**한다. Python 3.10 이상과 Tcl/Tk가 필요하다. Python 공식 설치 프로그램의 기본 설치에 포함된다. 이 컴퓨터에서는 Codex에 포함된 Python도 실행기가 찾는다.

1. 입력 형식을 고른다.
2. 같은 형식의 입력 파일을 하나 이상 선택한다.
3. 결과 폴더를 선택한다.
4. 필요하면 논리 용량, 원본 디스크, 시간 정렬·정밀도 옵션을 지정한다.
5. **변환 및 검증 시작**을 누른다.

각 파일마다 `.trace`와 `.trace.manifest.json`이 생성된다. 파일별로 별도 결과를 만들며 기존 결과를 덮어쓰지 않는다. 다른 폴더에서 가져온 동명 파일은 별도 결과 폴더를 사용한다.

명령행은 Windows·Linux·macOS에서 같은 방식으로 쓴다. Tcl/Tk는 필요 없다.

```text
python mqsim_trace_converter.py convert --format rocksdb-overwritezipf --input "input.csv" --output "output.trace"
python mqsim_trace_converter.py verify "output.trace"
python mqsim_trace_converter.py batch "examples/batch.example.json"
python -m unittest -v test_converter
```

Windows에서 `python` 대신 `py -3`도 사용할 수 있다. GUI를 직접 실행하려면 `python converter_gui.pyw`를 쓴다. Python 3.10~3.13에서 `.zst` 압축 원본을 읽을 때만 선택 의존성 `zstandard`가 필요하다. 설치: `python -m pip install zstandard`. `.csv`, `.vscsi`, `.gz`, `.bz2`, `.xz`는 외부 패키지를 사용하지 않는다.

## 지원 범위

| 자료 | `--format` | 실제 입력과 단위 |
|---|---|---|
| RocksDB overwritezipf | `rocksdb-overwritezipf` | `io_type,offset_secs,length_secs,ts`; 0=읽기, 1=쓰기; 512B 섹터; 시간 초 |
| RocksDB YCSB-A | `rocksdb-ycsba` | 위와 동일. `rocksdb`도 사용 가능 |
| MobileBlockIOTraces | `mobile` | `proces,device,rw_flag,sector,size,timestamp`; R/W; 512B 섹터; 시간 초. `process` 철자도 허용 |
| MSRC_1 | `msrc1` | `Timestamp,Hostname,DiskNumber,Type,Offset,Size,ResponseTime`; Read/Write; Offset/Size 바이트; 시간 FILETIME 100ns |
| MSRC_2 | `msrc2` | 위와 동일. `msrc`도 사용 가능 |
| CloudPhysics 원본 | `cloudphysics-vscsi` | 원본 little-endian VSCSI1(32B)/VSCSI2(40B) 자동 구분; LBN=512B 섹터, len=바이트, ts=마이크로초 |
| CloudPhysics CSV | `cloudphysics-csv` | 헤더 `timestamp,lbn,len,cmd,ver` 필수; timestamp=마이크로초, lbn=512B 섹터. `--length-unit bytes` 또는 `sectors` 필수 |

Mobile·CloudPhysics CSV는 헤더가 필수다. RocksDB·MSRC는 위 이름의 헤더가 있거나 없어도 된다. 헤더가 있으면 열 순서 변경을 허용한다. 빈 행은 요청으로 계산하지 않고 보고서에 센다. 나머지 비정상 행을 조용히 건너뛰지 않는다.

CloudPhysics는 SCSI READ/WRITE(6/10/12/16)를 지원한다. TRIM·FLUSH 등 다른 명령은 오류로 보고한다. 원본 요청을 읽기나 쓰기로 추측해 바꾸지 않는다.

**`cache_dataset-main`은 데이터 안내 저장소다. 실제 CloudPhysics 원본 파일을 별도로 확보해야 한다.** 공개 `cloudPhysicsIO.vscsi` 샘플로 원본 바이너리 파서를 검증했다. 전체 데이터셋의 실험 결과로 사용할 수 있는 표본이라는 뜻은 아니다.

**LCS와 oracleGeneral을 원본 트레이스로 받지 않는다.** cacheMon 미러의 파생 자료에는 4KiB 요청 분할·주소 변환·초 단위 시각 등 가공이 포함될 수 있다. oracleGeneral에는 읽기/쓰기 필드도 없다. 파일명만 바꿔서 입력하지 않는다. CloudPhysics CSV의 len 단위는 자료 설명에 불일치가 있어 사용자가 원본 형식을 확인하고 지정해야 한다.

## 변환 규칙

출력은 헤더 없는 ASCII/LF 파일이며 한 줄에 정확히 다섯 열이 들어간다.

```text
시간_ns 0 시작_LBA 요청_섹터수 요청종류
```

- MQSim 요청종류: **쓰기 0, 읽기 1**. RocksDB의 숫자 코드와 반대다.
- LBA와 요청 크기의 섹터 단위는 **항상 512바이트**다.
- 선택한 첫 요청 시각을 0으로 옮긴다. 나머지 요청 간격은 유지한다. 시간 계산에 이진 부동소수점을 사용하지 않는다.
- 기본적으로 원본 행 순서를 유지한다. 같은 시각의 요청을 합치거나 제거하지 않는다.
- 바이트 단위 입력은 주소와 크기가 512의 배수여야 한다. 크기 0, 알 수 없는 명령, 음수, 열 개수 오류, MQSim 정수 범위 초과, 손상된 압축 파일은 실패한다.
- `ResponseTime`과 `process`는 MQSim 입력 열이 아니므로 출력에서 제외한다. 원본 파일 해시와 필터·집계 정보는 보고서에 남긴다.
- 원본 디스크를 자동으로 합치지 않는다. 한 파일에 여러 디스크가 있으면 `--source-device`를 명시한다. 출력의 장치 번호는 단일 SSD 흐름을 나타내는 0이다. MQSim의 장치 열만으로 여러 디스크가 독립 분리되지 않는다.
- 재생 횟수는 1회다. 주소 재배치, 용량 축소, 요청 분할·병합, 임의 샘플링, 반복, 설치/실행 구간 연결, 사전 쓰기는 하지 않는다.

### 시간 정렬과 1ns 미만 값

시간이 역전된 파일은 기본적으로 실패한다. 필요하면 `--sort-time`을 명시한다. 디스크 기반 안정 정렬을 사용하며 같은 시각의 요청은 원본 순서를 유지한다. 정렬용 임시 파일은 결과 폴더 아래에 생성했다가 정리한다. 큰 파일은 추가 디스크 공간이 필요하다.

Mobile CSV의 일부 값은 `5218128.1812390005`처럼 1ns 미만 자리까지 있다. MQSim 정수 나노초에는 이를 그대로 표현할 수 없다.

- `--subnanosecond error`: 기본값. 정밀도 손실이 필요하면 실패한다.
- `--subnanosecond floor`: 절대 시각을 정수 ns로 내린 후 첫 요청을 0으로 옮긴다. 원본 시각당 손실은 1ns 미만이다.
- `--subnanosecond nearest`: 가장 가까운 정수 ns로 반올림한다. 정확히 중간이면 짝수로 반올림한다. 원본 시각당 오차는 최대 0.5ns다.

시간 처리 옵션과 영향받은 원본 행 수(`source.subnanosecond_rows`, 디스크 필터 적용 전)를 보고서에 기록한다. 팀이 공유하는 설정 파일에 동일한 옵션을 지정한다. Mobile 예:

```text
python mqsim_trace_converter.py convert --format mobile --input "diablo_exec.csv.gz" --output "diablo_exec.trace" --sort-time --subnanosecond floor
```

설치(`*_precond`)와 실행(`*_exec`)은 별도 파일로 변환한다. 두 파일을 연결하는 실험을 하려면 구간 경계와 시간 간격을 별도로 정의해야 한다.

### 디스크와 용량

```text
python mqsim_trace_converter.py convert --format msrc1 --input "hm_0.csv" --output "hm_0.trace" --source-device "hm:0" --capacity 256GB
python mqsim_trace_converter.py convert --format mobile --input "phone.csv.gz" --output "phone.trace" --source-device 8388608 --subnanosecond floor
python mqsim_trace_converter.py convert --format cloudphysics-csv --input "cloud.csv" --output "cloud.trace" --length-unit bytes
```

`--capacity`는 검사할 **호스트 논리 용량**이다. NAND 물리 용량이 아니다. OP를 제외한 실제 논리 용량을 입력한다. 미지정하면 주소를 그대로 보존하고 보고서의 `required_capacity_bytes`에 최소 필요 용량을 기록한다. 이때 `capacity: not_checked`로 표시된다.

- `256GB` = 256,000,000,000바이트
- `256GiB` = 274,877,906,944바이트

요청 **끝 주소**까지 검사한다. 용량을 초과하면 실패하며 modulo/folding을 적용하지 않는다. 변환 완료만으로 지정 SSD에서 주소가 유효하다는 뜻은 아니다. 실제 SSD 논리 용량과 비교해야 한다. 원본 MQSim은 범위 밖 LBA를 접어 넣는 코드가 있으므로 이 검사를 생략한 입력은 시뮬레이션 전에 특히 확인한다.

## 팀 공유와 재현

물리 256GiB·OP 7% 기준의 [추천 공용 설정 초안](profiles/review-256gib-op7-v1/README.md)을 검토할 수 있다. 검사용 논리 용량은 255,636,439,040B이며, 원본 선택·주소 초과 처리에 대한 미확정 항목을 포함한다.

`examples/batch.example.json`을 복사해 실제 경로와 실험에서 합의한 옵션을 채운다. 상대 경로는 **JSON 파일 위치**가 기준이다. 파일마다 source device, 시간 정렬, 1ns 미만 값 정책, 용량을 고정할 수 있다. 예제의 CloudPhysics 경로는 아직 없는 원본을 가리키는 자리표시자다.

팀에는 다음을 함께 공유한다.

1. 이 폴더 전체 또는 배포 ZIP과 버전
2. 원본 파일의 SHA-256
3. 실제 사용한 batch JSON
4. 변환된 `.trace`와 `.manifest.json`

보고서에는 실행 시각이나 컴퓨터의 절대 경로를 넣지 않는다. 같은 원본 파일명·바이트, 같은 변환기 코드, 같은 옵션이면 출력과 보고서가 동일하다. 컴퓨터마다 Python 버전이 달라도 정수·정확한 십진 계산과 안정 정렬 규칙을 사용한다. `.gz` 압축 설정이 다른 파일은 압축 해제 내용이 같아도 원본 압축 파일 해시가 달라질 수 있다.

검증 기록에는 전체/선택/디스크 필터 제외 요청 수와 읽기·쓰기 바이트 수, 선택 디스크, 기준 시각, 주소 범위, 원본/출력/변환기 해시가 들어간다. 변환 후 독립적인 두 번째 읽기로 출력 형식·요청 수·바이트 수를 검사한다. `verify`는 배포 후 출력 변조를 다시 검사한다. 원본 파일과의 의미 대응은 변환 시 검증하며 `verify`가 원본을 다시 변환하는 것은 아니다.

배치 실행은 실패한 작업을 보고하고 다음 파일을 처리한다. 하나라도 실패하면 종료 코드 1이다. 단일 변환/검증 오류는 종료 코드 2다. 성공은 0이다. 오류가 난 단일 파일의 부분 결과는 남기지 않는다. 강제 종료·전원 손실 때 남은 결과는 `verify`를 통과하기 전까지 완료 파일로 취급하지 않는다.

## MQSim에 입력

`workload.xml`의 `File_Path`를 결과 `.trace`로 지정하고, `Time_Unit`은 `NANOSECOND`, `Percentage_To_Be_Executed`는 `100`, `Relay_Count`는 `1`로 설정한다. 채널·칩·다이·플레인 할당은 실제 SSD 설정에 맞춘다.

초기 점유율, preconditioning, GC 정책, WL 임계값, 캐시 크기 등 시뮬레이션 설정은 이 변환기 밖에서 관리한다. 팀에서 같은 실험을 재현할 때는 같은 SHA-256의 변환 파일을 사용한다.

## 형식 근거

- MQSim: 저장소 `src/host/ASCII_Trace_Definition.h`, `IO_Flow_Trace_Based.cpp`; [공식 원본](https://github.com/CMU-SAFARI/MQSim).
- MSRC: 배포본 `MSRC_README.txt`; [Microsoft FILETIME 정의](https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ns-minwinbase-filetime), [원 논문](https://www.usenix.org/legacy/event/fast08/tech/full_papers/narayanan/narayanan.pdf).
- RocksDB: [Zenodo 데이터](https://zenodo.org/records/21455311), [제작자 소스 아카이브](https://zenodo.org/records/21789953)의 `block_ssd/tools/blktrace/blktrace.h` 및 `.c`. 0=Read, 1=Write, 주소/크기=섹터.
- Mobile: [MobileBlockIOTraces README](https://github.com/acsl-technion/MobileBlockIOTraces).
- VSCSI: [libCacheSim 원본 VSCSI 파서](https://github.com/1a1a11a/libCacheSim/blob/develop/libCacheSim/traceReader/customizedReader/vscsi.h), [공개 바이너리 샘플](https://github.com/1a1a11a/libCacheSim/blob/develop/data/cloudPhysicsIO.vscsi). 원본 바이너리의 len은 바이트로 취급한다. 캐시용 LCS/oracleGeneral 자료와 구분한다.
