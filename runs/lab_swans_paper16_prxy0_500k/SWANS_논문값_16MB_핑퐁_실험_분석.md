# SWANS 논문값 + 16MB zone 핑퐁 완화 실험

실험일: 2026-08-28

## 결론

연구실 SWANS 구현에 논문의 대표 설정 `th(5,15)`, 40초 epoch, 16MB zone, 256KB stripe를 적용하자, `prxy_0` 첫 50만 요청에서 migration은 6회에서 1회로 줄었고 같은 hot zone을 SSD 2와 SSD 3 사이에서 반복 이동하던 핑퐁은 4회에서 0회가 되었다. 순수 RAID-0 대비 write-sector 분포의 표준편차는 8.733pp에서 2.876pp로 67.1% 낮아졌고, 평균 응답시간은 RAID-0과 같은 42us였다.

따라서 현재 구현에서는 별도 cooldown 로직을 먼저 추가하기보다 논문 임계값의 넓은 간격을 적용하는 것이 핑퐁을 억제하는 가장 단순하고 효과적인 1차 수정이다.

## 논문 기준과 적용값

SWANS 논문은 `thprecautionary=5`, `thcritical=15`, 기본 epoch 40초, stripe 256KB를 사용한다. 8/16/32MB zone을 비교한 뒤 이후 실험에서는 성능과 메타데이터 비용의 절충으로 16MB를 사용한다. 논문은 두 임계값 사이를 넓게 두어 migration보다 저비용 data placement/redirect가 주로 발생하게 했으며, 다섯 실제 trace에서 migration은 trace당 최대 5회였다.

출처: [SWANS: An Interdisk Wear-Leveling Strategy for RAID-0 Structured SSD Arrays](https://casl.sdsu.edu/images/poster/pdf/SWANS.pdf)

| 항목 | 기존 연구실 설정 | 논문값 실험 |
|---|---:|---:|
| SSD 수 | 4 | 4 |
| Stripe | 512 LBA = 256KiB | 512 LBA = 256KiB |
| Zone | 16,384 LBA = 8MiB | 32,768 LBA = 16MiB |
| Default/placement/migration epoch | 40s | 40s |
| Precautionary threshold | 3 | 5 |
| Critical threshold | 6 | 15 |
| 동시 migration 수 | 1 | 1 |

NAND page 크기와 read/program/erase latency 등 MQSim 장치 모델은 동일하게 유지했다. 이번 비교는 SWANS 정책값과 zone 크기의 효과만 분리하기 위한 실험이다.

## 주 실험: prxy_0 첫 500K 요청, 원본 시간축

- 입력: MSRC `prxy_0.csv`의 처음 500,000개 유효 요청
- 주소 처리: 원래 global LBA 유지
- 시간 처리: 원본 시간축 유지(`time_acceleration=1`)
- 논문도 zone 크기 실험에서 각 trace의 첫 500K 요청을 사용한다.
- 세 설정 모두 같은 trace, SSD geometry, seed를 사용했다.
- 세 설정 모두 500,000개 요청을 전부 완료했다.

| 지표 | 기존 3/6, 8MB | 논문 5/15, 16MB | RAID-0 |
|---|---:|---:|---:|
| Epoch 평가 | 573 | 573 | 0 |
| Migration epoch/operation | 6 / 6 | **1 / 1** | 0 |
| Redirect operation | 129 | **97** | 0 |
| Migration 복사량 | 24,576 sector = 12MiB | **8,192 sector = 4MiB** | 0 |
| Background read/write | 6 / 6 | **2 / 2** | 0 / 0 |
| SSD write-sector 비율 | 27.326 / 21.926 / 23.735 / 27.013% | 29.612 / 21.926 / 25.003 / 23.459% | 39.212 / 21.926 / 15.455 / 23.407% |
| Write-sector 비율 표준편차 | **2.264pp** | 2.876pp | 8.733pp |
| 평균 응답시간 | 44us | **42us** | 42us |
| 최대 응답시간 | 5,847us | **5,320us** | 5,692us |
| Flash programmed bytes | 1,532,510,208 | **1,523,179,520** | 1,522,311,168 |
| 근사 flash WAF | 1.339431 | **1.331275** | 1.330517 |
| Mapping 중복 위치 | 0 | 0 | 0 |

### 핑퐁 확인

기존 3/6, 8MB 설정의 migration 순서는 다음과 같다.

| 시간 | Hot zone | 이동 |
|---:|---:|---|
| 40s | 84 | SSD 0 → 2 |
| 80s | 128 | SSD 0 → 2 |
| 120s | 128 | SSD 2 → 3 |
| 160s | 128 | SSD 3 → 2 |
| 200s | 128 | SSD 2 → 3 |
| 240s | 128 | SSD 3 → 2 |

zone 128이 SSD 2와 SSD 3 사이에서 네 번 방향을 뒤집었다. 반면 논문값 설정은 40초에 zone 40을 SSD 0에서 SSD 2로 한 번 옮긴 뒤 추가 migration이 없었다.

### 개선량과 대가

- Migration 횟수: 6 → 1, **83.3% 감소**
- Migration 복사량: 12MiB → 4MiB, **66.7% 감소**
- 동일 zone 역방향 핑퐁: 4 → 0, **완전 제거**
- 평균 응답시간: 44us → 42us, **4.5% 감소**
- 최대 응답시간: 5,847us → 5,320us, **9.0% 감소**
- WAF: 1.339431 → 1.331275, **0.61% 감소**
- RAID-0 대비 write-sector 불균형: 8.733pp → 2.876pp, **67.1% 감소**
- 기존 공격적 설정보다는 write-sector 표준편차가 2.264pp → 2.876pp로 커졌다. 즉 복사와 핑퐁을 줄이는 대신 최종 균형을 약간 덜 공격적으로 맞추는 절충이다.

## 보조 실험: 전체 src2_0 trace

기존 실험과 같은 16배 시간 압축 trace를 사용하여 1,557,814개 요청을 전부 처리했다.

| 지표 | 기존 3/6, 8MB | 논문 5/15, 16MB | RAID-0 |
|---|---:|---:|---:|
| Epoch 평가 | 1,092 | 1,092 | 0 |
| Migration operation | 1,092 | **0** | 0 |
| Redirect operation | 0 | **114** | 0 |
| Migration 복사량 | 17,715,200 sector = 8.45GiB | **0** | 0 |
| Write-sector 비율 표준편차 | 3.863pp | 3.888pp | 6.945pp |
| 평균 응답시간 | 203us | 222us | 199us |
| 최대 응답시간 | 47,283us | 51,962us | 28,257us |
| Flash programmed bytes | 19,989,905,408 | **10,884,366,336** | 10,885,595,136 |
| 근사 flash WAF | 1.993316 | **1.085347** | 1.085469 |
| Mapping 중복 위치 | 0 | 0 | 0 |

이 trace에서는 첫 μ가 14.83으로 critical 15 바로 아래였기 때문에 논문값 설정은 migration 대신 redirect만 수행했다. 그 결과 기존 설정의 매-epoch migration 핑퐁과 8.45GiB 복사가 사라졌고, WAF는 RAID-0 수준으로 돌아왔다. 다만 평균 응답시간은 RAID-0보다 11.6%, 기존 공격적 SWANS보다 9.4% 높았다. 이 trace에서는 redirect로 바뀐 zone 배치가 순간 부하를 늘린 것으로 보이며, 복사 제거가 항상 latency 개선으로 이어지지는 않았다.

## 해석 시 주의점

1. XML의 `SWANS_Last_Mu`는 연구실 정책 코드가 누적 write subrequest 수의 SSD별 비율로 계산한다. 위 표의 별도 균형 값은 실제 attributed write sector 비율로 다시 계산했으므로 두 값은 같지 않다.
2. 두 trace 모두 GC, SSD 내부 WL, erase가 0회였다. 따라서 이번 결과는 실제 수명 연장값이 아니라 migration 복사량, flash programmed bytes, 근사 WAF를 이용한 단기 proxy 평가다.
3. 16MB는 zone의 최대 범위다. migration 시에는 zone 전체를 무조건 복사하지 않고 written-block bitmap에 표시된 block만 복사하므로 실제 복사량은 4MiB였다.
4. 이번 결과에서는 논문 임계값만으로 핑퐁이 제거되었다. cooldown이나 최소 개선량 guard 같은 알고리즘 변경은 아직 넣지 않았다.

## 권장 기본값

현재 연구실 SWANS 구현의 다음 실험 기본값으로 아래 설정을 권장한다.

```xml
<SSD_Count>4</SSD_Count>
<Stripe_Unit_LBA>512</Stripe_Unit_LBA>
<SWANS_Zone_Size_LBA>32768</SWANS_Zone_Size_LBA>
<SWANS_Epoch_Default>40000000000</SWANS_Epoch_Default>
<SWANS_Epoch_Placement>40000000000</SWANS_Epoch_Placement>
<SWANS_Epoch_Migration>40000000000</SWANS_Epoch_Migration>
<SWANS_TH_Precautionary>5</SWANS_TH_Precautionary>
<SWANS_TH_Critical>15</SWANS_TH_Critical>
<SWANS_Max_Concurrent_Migrations>1</SWANS_Max_Concurrent_Migrations>
```

향후 여러 trace에서도 동일 zone의 역방향 이동이 다시 관찰될 때만 `migration cooldown` 또는 `predicted μ improvement guard`를 추가하는 것이 좋다.

## 재현 파일

- `ssdconfig_paper16.xml`: 논문값 + 16MB 설정
- `ssdconfig_aggressive8.xml`: 기존 3/6 + 8MB 비교 설정
- `ssdconfig_raid0.xml`: SWANS 비활성 baseline
- `prxy0_500k_realtime.trace`: 원본 시간축 500K trace
- `workload_prxy0_500k_paper16_realtime_scenario_1.xml`: 논문값 결과
- `workload_prxy0_500k_aggressive8_realtime_scenario_1.xml`: 기존 설정 결과
- `workload_prxy0_500k_raid0_realtime_scenario_1.xml`: RAID-0 결과
