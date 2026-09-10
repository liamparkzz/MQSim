# 001~004 후속 수정 코드와 설명

- 작성일: 2026-09-10 (Asia/Seoul), 노트북.
- 기준 코드: PR79 통합본 `7d881b9126bcc21f307a3ab8e18be76b292ed522`.
- 상태: 코드 수정 완료 / 사용자 동작 검증 대기. 컴파일·링크만 확인하며 시뮬레이션·실험 설계·새 검증 코드는 포함하지 않는다.
- 범위: 관련 소스 6개 파일, 변경 블록 12개. 설정·workload·테스트·빌드/동기화 스크립트와 관련 없는 소스는 변경하지 않는다. 최초 Obsidian Baseline은 보존한다.

| 번호 | 새 수정본에 포함한 처리 | 이번 변경 |
|---|---|---|
| 001 XML 설정 전달 | GC/WL 생성자에 동적 WL·정적 WL·임계값·Seed 전달 | PR79 통합본의 `src/exec/SSD_Device.cpp:311` 유지 |
| 002 마모 차이 | 최대·최소 Erase_count 차이 반환 | 반환식만 수정 |
| 003 barrier 요청 보존 | 대기 요청 재제출, 공간 부족 재개 시 잠금/CMT 재확인, 이동 목적지 보호, 정적 WL 대상 주소 보정 | 아래 11개 관련 변경 블록 |
| 004 매핑 쓰기 처리 | 두 OutOfOrder 스케줄러의 매핑 쓰기 큐 선택, update-read와 write 양방향 연결 및 읽기 완료 시 의존성 해제 | PR79 통합본의 해당 로직 유지 |

003 변경은 코드에서 확인한 잘못된 경로를 수정한 것이다. 과거 LPA 149 잔류·215/125 중복 잠금이 이 수정본에서 없어졌다는 실행 결과는 아직 없다. 001~004를 모두 만족한다는 최종 판정은 사용자 검증 후 기록한다. 과거 PR79의 통과 수치를 새 수정본의 검증 결과로 재사용하지 않는다.

## 변경 코드와 블록별 설명

### 01. 매핑 페이지 이동 여부 전달 — allocate_page_in_plane_for_translation_write

파일: `src/ssd/Address_Mapping_Unit_Page_Level.cpp` · 수정본 시작 행: `1227`

```diff
@@ -1227,7 +1227,7 @@ namespace SSD_Components
 			block_manager->Invalidate_page_in_block(transaction->Stream_id, prevAddr);
 		}
 
-		block_manager->Allocate_block_and_page_in_plane_for_translation_write(transaction->Stream_id, transaction->Address, false);
+		block_manager->Allocate_block_and_page_in_plane_for_translation_write(transaction->Stream_id, transaction->Address, is_for_gc);
 		transaction->PPA = Convert_address_to_ppa(transaction->Address);
 		domain->GlobalTranslationDirectory[mvpn].MPPN = (MPPN_type)transaction->PPA;
 		domain->GlobalTranslationDirectory[mvpn].TimeStamp = CurrentTimeStamp;
```

**수정 이유:** 상위 함수는 GC/WL 이동 여부를 알고 있었지만 하위 할당 함수에는 항상 false를 넘겼다. 매핑 이동을 일반 매핑 쓰기로 집계하면 사용자 program 카운터가 완료 후에도 남을 수 있다.

**코드 동작 설명:** false 상수 대신 is_for_gc를 전달한다. 일반 매핑 쓰기는 기존 사용자 program 카운터를 사용하고, GC/WL 매핑 이동은 아래의 전용 카운터를 사용한다. 하위 함수에 이미 있는 GC 중 재귀 Check_gc_required 억제 조건에도 실제 이동 여부가 전달된다.

**검증 범위:** 코드 검토와 전체 MSVC 컴파일·링크만 확인. 동작 검증·시뮬레이션은 미실행이며 사용자 검증 대기.

### 02. LPA barrier 해제 후 실제 요청 재제출 — Remove_barrier_for_accessing_lpa

파일: `src/ssd/Address_Mapping_Unit_Page_Level.cpp` · 수정본 시작 행: `1818`

```diff
@@ -1818,23 +1818,22 @@ namespace SSD_Components
 		}
 		domains[stream_id]->Locked_LPAs.erase(itr);
 
-		//If there are read requests waiting behind the barrier, then MQSim assumes they can be serviced with the actual page data that is accessed during GC execution
+		//Detach waiting requests before dispatch, which may start GC and create another barrier.
+		std::list<NVM_Transaction*> waiting_transactions;
 		auto read_tr = domains[stream_id]->Read_transactions_behind_LPA_barrier.find(lpa);
 		while (read_tr != domains[stream_id]->Read_transactions_behind_LPA_barrier.end()) {
-			connected_transaction_serviced_signal_handler((*read_tr).second);
-			delete (*read_tr).second;
+			waiting_transactions.push_back(read_tr->second);
 			domains[stream_id]->Read_transactions_behind_LPA_barrier.erase(read_tr);
 			read_tr = domains[stream_id]->Read_transactions_behind_LPA_barrier.find(lpa);
 		}
 
-		//If there are write requests waiting behind the barrier, then MQSim assumes they can be serviced with the actual page data that is accessed during GC execution. This may not be 100% true for all write requests, but, to avoid more complexity in the simulation, we accept this assumption.
 		auto write_tr = domains[stream_id]->Write_transactions_behind_LPA_barrier.find(lpa);
 		while (write_tr != domains[stream_id]->Write_transactions_behind_LPA_barrier.end()) {
-			connected_transaction_serviced_signal_handler((*write_tr).second);
-			delete (*write_tr).second;
+			waiting_transactions.push_back(write_tr->second);
 			domains[stream_id]->Write_transactions_behind_LPA_barrier.erase(write_tr);
 			write_tr = domains[stream_id]->Write_transactions_behind_LPA_barrier.find(lpa);
 		}
+		Translate_lpa_to_ppa_and_dispatch(waiting_transactions);
 	}
 
 	inline void Address_Mapping_Unit_Page_Level::Remove_barrier_for_accessing_mvpn(stream_id_type stream_id, MVPN_type mvpn)
```

**수정 이유:** PR79는 캐시 완료 콜백을 호출한 뒤 대기 트랜잭션을 삭제했다. 특히 대기 쓰기는 실제 페이지 할당·program 없이 완료된 것으로 처리되므로 정상 쓰기 경로를 거쳐야 한다.

**코드 동작 설명:** 잠금을 해제한 다음 해당 LPA의 읽기와 쓰기 포인터를 waiting_transactions에 모으고 기존 barrier 큐에서 먼저 뺀다. 트랜잭션과 Host 요청은 삭제하지 않는다. 마지막에 Translate_lpa_to_ppa_and_dispatch를 호출해 현재 CMT·LPA 잠금·공간 상태를 다시 확인하고, 필요한 flash 읽기/쓰기와 부분 쓰기의 update-read를 제출한다. 완료 통지는 실제 PHY 완료 경로가 담당한다. 재제출 도중 새 잠금이 생기면 요청은 다시 barrier 큐에 보존된다. 목록을 먼저 분리하므로 이 함수가 새 대기 요청까지 반복해서 가져오지 않는다. 기존처럼 읽기 묶음 뒤에 쓰기 묶음을 처리하며 새로운 전역 I/O 순서 정책은 추가하지 않는다.

**검증 범위:** 코드 검토와 전체 MSVC 컴파일·링크만 확인. 동작 검증·시뮬레이션은 미실행이며 사용자 검증 대기.

### 03. 공간 부족 대기 쓰기의 잠금·CMT 재확인 — Start_servicing_writes_for_overfull_plane

파일: `src/ssd/Address_Mapping_Unit_Page_Level.cpp` · 수정본 시작 행: `1931`

```diff
@@ -1932,20 +1931,9 @@ namespace SSD_Components
 	{
 		std::set<NVM_Transaction_Flash_WR*>& waiting_write_list = Write_transactions_for_overfull_planes[plane_address.ChannelID][plane_address.ChipID][plane_address.DieID][plane_address.PlaneID];
 
-		ftl->TSU->Prepare_for_transaction_submit();
-		auto program = waiting_write_list.begin();
-		while (program != waiting_write_list.end()) {
-			if (translate_lpa_to_ppa((*program)->Stream_id, *program)) {
-				ftl->TSU->Submit_transaction(*program);
-				if ((*program)->RelatedRead != NULL) {
-					ftl->TSU->Submit_transaction((*program)->RelatedRead);
-				}
-				waiting_write_list.erase(program++);
-			}
-			else {
-				break;
-			}
-		}
-		ftl->TSU->Schedule();
+		//Recheck LPA barriers and CMT entries after waiting for free space.
+		std::list<NVM_Transaction*> waiting_transactions(waiting_write_list.begin(), waiting_write_list.end());
+		waiting_write_list.clear();
+		Translate_lpa_to_ppa_and_dispatch(waiting_transactions);
 	}
 }
```

**수정 이유:** 대기 중 새 GC가 시작될 수 있는데 기존 재개 경로는 잠금을 검사하지 않고 translate_lpa_to_ppa를 직접 호출했다. 잠긴 원본 페이지가 덮어쓰기로 무효화되면 해당 LPA의 이동·해제 경로가 사라질 수 있다. 대기 중 CMT 엔트리가 퇴거될 수도 있다.

**코드 동작 설명:** 기존 set의 포인터를 별도 목록에 복사하고 set을 비운 뒤 공통 주소 변환·제출 진입점을 호출한다. 잠긴 요청은 barrier 큐로, CMT miss 요청은 매핑 대기 큐로, 여전히 공간이 부족한 요청은 overfull 큐로 돌아간다. 요청 객체를 삭제하지 않으며 공통 진입점이 쓰기와 RelatedRead 제출을 처리한다. 첫 공간 부족 요청에서 중단하던 방식 대신 분리한 요청 각각의 현재 대기 사유를 다시 판단한다. 재시도에 따른 CMT 조회 통계와 처리 시각은 달라질 수 있다.

**검증 범위:** 코드 검토와 전체 MSVC 컴파일·링크만 확인. 동작 검증·시뮬레이션은 미실행이며 사용자 검증 대기.

### 04. GC 데이터 이동 목적지의 program 수 증가 — Allocate_block_and_page_in_plane_for_gc_write

파일: `src/ssd/Flash_Block_Manager.cpp` · 수정본 시작 행: `43`

```diff
@@ -43,6 +43,7 @@ namespace SSD_Components
 		plane_record->Free_pages_count--;		
 		page_address.BlockID = plane_record->GC_wf[stream_id]->BlockID;
 		page_address.PageID = plane_record->GC_wf[stream_id]->Current_page_write_index++;
+		plane_record->Blocks[page_address.BlockID].Ongoing_gc_program_count++;
 
 		
 		//The current write frontier block is written to the end
```

**수정 이유:** GC 목적지에는 사용자 program 수가 증가하지 않았다. 목적지 frontier가 꽉 차서 교체되면 아직 쓰는 중인 블록이 새 GC 후보로 보일 수 있고, 이동 중인 LPA를 다시 잠글 수 있다.

**코드 동작 설명:** 할당한 실제 목적지 블록의 Ongoing_gc_program_count를 1 증가시킨다. frontier 교체와 Check_gc_required 호출보다 먼저 증가시키므로 재귀적으로 후보를 선택해도 진행 중인 목적지를 제외할 수 있다. 기존 유효·빈 페이지 집계는 유지한다.

**검증 범위:** 코드 검토와 전체 MSVC 컴파일·링크만 확인. 동작 검증·시뮬레이션은 미실행이며 사용자 검증 대기.

### 05. 일반 매핑 쓰기와 GC 매핑 이동의 카운터 분리 — Allocate_block_and_page_in_plane_for_translation_write

파일: `src/ssd/Flash_Block_Manager.cpp` · 수정본 시작 행: `96`

```diff
@@ -95,7 +96,11 @@ namespace SSD_Components
 		plane_record->Free_pages_count--;
 		page_address.BlockID = plane_record->Translation_wf[streamID]->BlockID;
 		page_address.PageID = plane_record->Translation_wf[streamID]->Current_page_write_index++;
-		program_transaction_issued(page_address);
+		if (is_for_gc) {
+			plane_record->Blocks[page_address.BlockID].Ongoing_gc_program_count++;
+		} else {
+			program_transaction_issued(page_address);
+		}
 
 		//The current write frontier block for translation pages is written to the end
 		if (plane_record->Translation_wf[streamID]->Current_page_write_index == pages_no_per_block) {
```

**수정 이유:** 일반 매핑 쓰기는 USERIO/MAPPING 완료 분기에서 사용자 카운터를 내리지만, GC/WL 이동은 다른 완료 분기를 통과한다. 모두 사용자 카운터에 더하면 증가·감소가 짝을 이루지 않는다.

**코드 동작 설명:** is_for_gc가 true이면 새 GC program 카운터를 증가시킨다. false이면 기존 program_transaction_issued 호출을 그대로 수행한다. 일반 매핑 쓰기의 스케줄링 정책은 바꾸지 않고 이동 목적지 보호에 필요한 집계만 분리한다.

**검증 범위:** 코드 검토와 전체 MSVC 컴파일·링크만 확인. 동작 검증·시뮬레이션은 미실행이며 사용자 검증 대기.

### 06. 블록 번호 대신 삭제 횟수 차이 반환 — Get_min_max_erase_difference

파일: `src/ssd/Flash_Block_Manager_Base.cpp` · 수정본 시작 행: `158`

```diff
@@ -158,7 +158,7 @@ namespace SSD_Components
 			}
 		}
 
-		return max_erased_block - min_erased_block;
+		return plane_record->Blocks[max_erased_block].Erase_count - plane_record->Blocks[min_erased_block].Erase_count;
 	}
 
 	flash_block_ID_type Flash_Block_Manager_Base::Get_coldest_block_id(const NVM::FlashMemory::Physical_Page_Address& plane_address)
```

**수정 이유:** 최대·최소 삭제 횟수의 블록을 찾은 뒤 블록 번호끼리 빼고 있었다. 번호 배치에 따라 차이가 왜곡되거나 unsigned 뺄셈이 순환했다.

**코드 동작 설명:** 탐색 결과인 두 배열 인덱스로 Blocks의 Erase_count를 읽어 최대 횟수에서 최소 횟수를 뺀다. 탐색 루프, 변수 이름, 정적 WL 활성화 여부와 차이 >= 임계값 조건은 유지한다. 반환값의 단위가 실제 삭제 횟수가 된다.

**검증 범위:** 코드 검토와 전체 MSVC 컴파일·링크만 확인. 동작 검증·시뮬레이션은 미실행이며 사용자 검증 대기.

### 07. GC/WL 이동 목적지 전용 카운터 — Block_Pool_Slot_Type

파일: `src/ssd/Flash_Block_Manager_Base.h` · 수정본 시작 행: `43`

```diff
@@ -43,6 +43,7 @@ namespace SSD_Components
 		bool Hot_block = false;//Used for hot/cold separation mentioned in the "On the necessity of hot and cold data identification to reduce the write amplification in flash-based SSDs", Perf. Eval., 2014.
 		int Ongoing_user_read_count;
 		int Ongoing_user_program_count;
+		int Ongoing_gc_program_count = 0;//Protect GC/WL destinations until their page programs complete.
 		void Erase();
 	};
```

**수정 이유:** 원본 구조에는 사용자 읽기·program 수만 있어 GC 이동 목적지의 진행 중 program을 별도로 나타낼 수 없었다.

**코드 동작 설명:** Ongoing_gc_program_count를 블록마다 추가하고 생성 시 0으로 초기화한다. 데이터/매핑 이동 할당에서 증가하고 GC/WL 쓰기 완료에서 감소한다. 사용자 카운터의 의미를 유지하고 후보 검사에 이 카운터를 함께 사용한다. 공개 설정이나 파일 형식은 추가하지 않는다.

**검증 범위:** 코드 검토와 전체 MSVC 컴파일·링크만 확인. 동작 검증·시뮬레이션은 미실행이며 사용자 검증 대기.

### 08. GC/WL 쓰기 완료 시 목적지 카운터 감소 — handle_transaction_serviced_signal_from_PHY

파일: `src/ssd/GC_and_WL_Unit_Base.cpp` · 수정본 시작 행: `144`

```diff
@@ -144,6 +144,7 @@ namespace SSD_Components
 				break;
 			}
 			case Transaction_Type::WRITE:
+				pbke->Blocks[transaction->Address.BlockID].Ongoing_gc_program_count--;
 				if (pbke->Blocks[((NVM_Transaction_Flash_WR*)transaction)->RelatedErase->Address.BlockID].Holds_mapping_data) {
 					_my_instance->address_mapping_unit->Remove_barrier_for_accessing_mvpn(transaction->Stream_id, (MVPN_type)transaction->LPA);
 					DEBUG(Simulator->Time() << ": MVPN=" << (MVPN_type)transaction->LPA << " unlocked!!");
```

**수정 이유:** 할당 시 증가시킨 이동 program 수를 실제 완료 시점에 해제해야 목적지가 계속 후보에서 제외되지 않는다.

**코드 동작 설명:** USERIO/MAPPING/CACHE 처리가 return한 뒤의 GC/WL WRITE 분기에서 실제 목적지 transaction->Address의 카운터를 1 감소시킨다. RelatedErase가 가리키는 원본 블록의 수를 내리는 것이 아니다. 바로 이어서 기존 LPA/MVPN 잠금 해제를 수행한다. 잠금 해제에 따른 재제출이 새 GC를 유발하더라도 완료된 쓰기와 남은 이동 쓰기 수를 기준으로 후보를 판단한다.

**검증 범위:** 코드 검토와 전체 MSVC 컴파일·링크만 확인. 동작 검증·시뮬레이션은 미실행이며 사용자 검증 대기.

### 09. 이동 중인 목적지의 재선택 차단 — is_safe_gc_wl_candidate

파일: `src/ssd/GC_and_WL_Unit_Base.cpp` · 수정본 시작 행: `240`

```diff
@@ -239,7 +240,8 @@ namespace SSD_Components
 		}
 
 		//The block shouldn't have an ongoing program request (all pages must already be written)
-		if (plane_record->Blocks[gc_wl_candidate_block_id].Ongoing_user_program_count > 0) {
+		if (plane_record->Blocks[gc_wl_candidate_block_id].Ongoing_user_program_count > 0
+			|| plane_record->Blocks[gc_wl_candidate_block_id].Ongoing_gc_program_count > 0) {
 			return false;
 		}
```

**수정 이유:** frontier와 사용자 program만 검사하면 frontier에서 벗어났지만 GC/WL program이 남은 목적지를 다시 선택할 수 있다.

**코드 동작 설명:** 기존 사용자 program > 0 조건에 GC program > 0을 OR로 추가한다. 둘 중 하나라도 남으면 false를 반환한다. 기존 frontier 제외와 Has_ongoing_gc_wl 검사는 그대로 둔다. LPA 중복 잠금 오류를 무시하거나 lock을 덮어쓰는 처리는 추가하지 않는다.

**검증 범위:** 코드 검토와 전체 MSVC 컴파일·링크만 확인. 동작 검증·시뮬레이션은 미실행이며 사용자 검증 대기.

### 10. 정적 WL 상태를 실제 후보 주소에 설정 — run_static_wearleveling

파일: `src/ssd/GC_and_WL_Unit_Base.cpp` · 수정본 시작 행: `270`

```diff
@@ -268,7 +270,7 @@ namespace SSD_Components
 		Block_Pool_Slot_Type* block = &pbke->Blocks[wl_candidate_block_id];
 
 		//Run the state machine to protect against race condition
-		block_manager->GC_WL_started(wl_candidate_block_id);
+		block_manager->GC_WL_started(wl_candidate_address);
 		pbke->Ongoing_erase_operations.insert(wl_candidate_block_id);
 		address_mapping_unit->Set_barrier_for_accessing_physical_block(wl_candidate_address);//Lock the block, so no user request can intervene while the GC is progressing
 		if (block_manager->Can_execute_gc_wl(wl_candidate_address)) {//If there are ongoing requests targeting the candidate block, the gc execution should be postponed
```

**수정 이유:** GC_WL_started는 Physical_Page_Address를 받는데 블록 번호만 전달했다. 암시적 생성자는 그 값을 ChannelID로 해석하고 BlockID는 0으로 두므로 엉뚱한 위치를 표시하거나 범위를 벗어날 수 있다.

**코드 동작 설명:** 직전에 구성한 wl_candidate_address 전체를 전달한다. 채널·칩·다이·플레인과 후보 BlockID가 보존되어 상태 플래그, ongoing-erase 집합, barrier가 같은 실제 후보를 가리킨다. 진행 중 읽기 때문에 미뤄진 WL도 이후 완료 이벤트가 해당 블록의 플래그를 확인할 수 있다. WL 정책이나 실행 통계 정의를 변경하지 않는다.

**검증 범위:** 코드 검토와 전체 MSVC 컴파일·링크만 확인. 동작 검증·시뮬레이션은 미실행이며 사용자 검증 대기.

### 11. GREEDY의 안전하지 않은 초기 후보 교체 — Check_gc_required

파일: `src/ssd/GC_and_WL_Unit_Page_Level.cpp` · 수정본 시작 행: `58`

```diff
@@ -58,7 +58,8 @@ namespace SSD_Components
 						gc_candidate_block_id++;
 					}
 					for (flash_block_ID_type block_id = 1; block_id < block_no_per_plane; block_id++) {
-						if (pbke->Blocks[block_id].Invalid_page_count > pbke->Blocks[gc_candidate_block_id].Invalid_page_count
+						if ((!is_safe_gc_wl_candidate(pbke, gc_candidate_block_id)
+							|| pbke->Blocks[block_id].Invalid_page_count > pbke->Blocks[gc_candidate_block_id].Invalid_page_count)
 							&& pbke->Blocks[block_id].Current_page_write_index == pages_no_per_block
 							&& is_safe_gc_wl_candidate(pbke, block_id)) {
 							gc_candidate_block_id = block_id;
```

**수정 이유:** 초기 후보가 이동 중인 목적지 등 부적합한 블록이어도 invalid page 수가 많으면 뒤의 안전한 후보로 바뀌지 않았다. 최종 안전 검사만 강화하면 해당 초기 후보 때문에 이번 GC 호출이 끝나고 정상 후보를 놓칠 수 있다.

**코드 동작 설명:** 현재 후보가 안전하지 않으면 invalid page 수 비교와 무관하게 뒤의 안전한 full block으로 교체할 수 있게 한다. 현재 후보도 안전하다면 기존처럼 invalid page 수가 더 큰 full block만 선택한다. 교체 대상의 full block 조건과 안전 검사는 그대로 유지한다. GREEDY의 유효 후보 간 점수 기준은 변경하지 않는다.

**검증 범위:** 코드 검토와 전체 MSVC 컴파일·링크만 확인. 동작 검증·시뮬레이션은 미실행이며 사용자 검증 대기.

### 12. 모든 선택 정책의 최종 후보 검사 — Check_gc_required

파일: `src/ssd/GC_and_WL_Unit_Page_Level.cpp` · 수정본 시작 행: `132`

```diff
@@ -131,7 +132,8 @@ namespace SSD_Components
 			}
 
 			//This should never happen, but we check it here for safty
-			if (pbke->Ongoing_erase_operations.find(gc_candidate_block_id) != pbke->Ongoing_erase_operations.end()) {
+			if (pbke->Ongoing_erase_operations.find(gc_candidate_block_id) != pbke->Ongoing_erase_operations.end()
+				|| !is_safe_gc_wl_candidate(pbke, gc_candidate_block_id)) {
 				return;
 			}
```

**수정 이유:** 일부 정책은 초기 후보, 재시도 한도 또는 FIFO 선택을 통해 안전 검사에 통과하지 않은 블록을 최종 후보로 남길 수 있다. 앞서 추가한 이동 목적지 보호가 실제 잠금 직전에도 적용되어야 한다.

**코드 동작 설명:** 기존 ongoing-erase 중복 검사에 !is_safe_gc_wl_candidate를 추가한다. frontier·진행 중 program·이미 GC/WL 중인 블록이면 상태 설정과 barrier 생성 전에 반환한다. 정책별 점수·난수·후보 선택 방법은 유지한다. 이번 호출에서 안전하지 않은 후보를 억지로 실행하거나 다른 정책으로 대체하지 않는다.

**검증 범위:** 코드 검토와 전체 MSVC 컴파일·링크만 확인. 동작 검증·시뮬레이션은 미실행이며 사용자 검증 대기.

## 확인 범위와 인계

- 컴파일 환경: MSVC 14.51, x64, C++14, `/Od /MD`. 일반 시뮬레이터를 빌드만 하며 실행하지 않는다. 기존 소스의 컴파일 경고는 별도 동작 검증을 뜻하지 않는다.
- 로컬 빌드 기록: `C:/CODEX/MQsimExperiments/20260910-issues001-004-code/compile.log`, 최종 GREEDY 보정의 `compile-gc-final.log`, `link.log`. 전체 컴파일 뒤 마지막으로 바뀐 GC 소스만 재컴파일하고 전체 객체를 다시 링크했다. 빌드 산출물은 GitHub 코드 저장소에 넣지 않는다.
- 새 테스트·계측·검증 시나리오를 작성하지 않았다. 다음 작업은 사용자가 설계한 001~004 검증이다.
- 이번 작업에서 COPYBACK 경로, 캐시 정책, GC 정책별 선택 알고리즘, WL 통계 집계의 기존 별도 문제를 포괄 수정하지 않았다. 관련 기록의 기본 일반 읽기→쓰기 GC 이동 경로를 중심으로 수정했으며 다른 모드의 성공을 주장하지 않는다.
- GitHub 백업 커밋에서 Obsidian Current 전체 코드를 내보내고, 해당 commit 이력 노트에 이 페이지와 수정 일시·커밋 링크를 함께 보존한다.
