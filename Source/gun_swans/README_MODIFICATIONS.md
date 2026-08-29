# OS_RAID_PROJ 수정본: WAM + no-cache + OP 20% + endurance 종료

이 폴더는 연구실 `OS_RAID_PROJ`를 직접 덮어쓰지 않고 만든 별도 수정본이다.

## 반영 사항

1. WAM 누적 쓰기량
   - host write는 RAID 분할 및 SWANS redirection이 끝난 **최종 SSD**에 sector 단위로 누적한다.
   - migration restore write도 실제 복사 sector 수만큼 목적 SSD에 누적한다.
   - `WAM_Cumulative_Write_Sectors = WAM_Host_Write_Sectors + WAM_Migration_Write_Sectors`이다.
   - logical zone을 옮겨도 과거의 물리 wear는 다른 SSD로 이전하지 않는다.

2. 캐시 비활성화와 migration 대기
   - 기본 workload의 `Device_Level_Data_Caching_Mode`는 모두 `TURNED_OFF`이다.
   - migration 대상 zone의 read와 write는 모두 migration queue에서 기다린 뒤 SSD backend로 replay된다.
   - write를 RAM에서 완료시키는 `Complete_without_dispatch` 경로는 사용하지 않는다.

3. bad block과 OP 수명 종료
   - erase 완료 후 `Erase_count >= Block_PE_Cycles_Limit`이면 block을 bad로 표시한다.
   - bad block은 free block pool과 이후 GC/WL 후보에서 제외한다.
   - OP 예비 block 한도는 `ceil(전체 물리 block 수 × Overprovisioning_Ratio)`이다.
   - 완료된 bad block과 이미 P/E 한계 도달이 확정된 in-flight erase의 합이 OP 한도에 도달하면 시뮬레이션을 종료한다.

4. 기본 실험 설정
   - OP: 20%
   - SWANS zone: 32,768 LBA = 16 MiB (512-byte sector 기준)
   - SWANS threshold: precautionary 5, critical 15
   - workload SSD cache mode: off

## 빌드

Visual Studio 2022 Community C++ 도구가 설치된 Windows에서:

```powershell
.\build_msvc.bat
```

생성 파일:

```text
build_msvc\MQSim_wam_nocache_op20.exe
```

검증한 바이너리 SHA-256:

```text
9DD5F5FB09A325744896E88DCDA4399C875052431C858AFCED5E4168D99EE0E9
```

## 주요 변경 파일

- `src/policy/wear_leveling_policy.*`: sector 단위 host/migration/cumulative WAM
- `src/policy/zone_directory.*`: zone write 양을 sector 단위로 기록
- `src/policy/migration_executor.cpp`: read/write 공통 대기 및 backend replay
- `src/exec/RAID_Controller.cpp`: 최종 배치 SSD와 migration restore write 관측
- `src/ssd/Flash_Block_Manager*`: bad block 격리, pending retirement, OP 한도 종료
- `src/ssd/GC_and_WL_Unit*`: bad block 제외와 종료 후 후속 GC 차단
- `src/exec/RAID_Device.cpp`: WAM/bad block/OP 결과 출력
- `ssdconfig.xml`, `workload.xml`: OP 20%, 16 MiB zone, cache off 기본값

## 실험 결과 위치

- 실제 MSRC 50만 요청: `..\..\runs\lab_swans_wam_nocache_op20_prxy0_500k`
- migration 및 read/write wait 검증: `..\..\runs\lab_swans_wam_nocache_op20_migration_check`
- bad block/OP 종료 검증: `..\..\runs\lab_swans_wam_nocache_op20_endurance_check`

