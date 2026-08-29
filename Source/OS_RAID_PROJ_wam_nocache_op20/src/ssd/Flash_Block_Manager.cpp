
#include <algorithm>

#include "../nvm_chip/flash_memory/Physical_Page_Address.h"
#include "../sim/Engine.h"
#include "Flash_Block_Manager.h"
#include "Stats.h"

namespace SSD_Components
{
	Flash_Block_Manager::Flash_Block_Manager(GC_and_WL_Unit_Base* gc_and_wl_unit, unsigned int max_allowed_block_erase_count, unsigned int total_concurrent_streams_no,
		unsigned int channel_count, unsigned int chip_no_per_channel, unsigned int die_no_per_chip, unsigned int plane_no_per_die,
		unsigned int block_no_per_plane, unsigned int page_no_per_block, double overprovisioning_ratio)
		: Flash_Block_Manager_Base(gc_and_wl_unit, max_allowed_block_erase_count, total_concurrent_streams_no, channel_count, chip_no_per_channel, die_no_per_chip,
			plane_no_per_die, block_no_per_plane, page_no_per_block, overprovisioning_ratio)
	{
	}

	Flash_Block_Manager::~Flash_Block_Manager()
	{
	}

	void Flash_Block_Manager::Allocate_block_and_page_in_plane_for_user_write(const stream_id_type stream_id, NVM::FlashMemory::Physical_Page_Address& page_address)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Valid_pages_count++;
		plane_record->Free_pages_count--;		
		page_address.BlockID = plane_record->Data_wf[stream_id]->BlockID;
		page_address.PageID = plane_record->Data_wf[stream_id]->Current_page_write_index++;
		program_transaction_issued(page_address);

		//The current write frontier block is written to the end
		if(plane_record->Data_wf[stream_id]->Current_page_write_index == pages_no_per_block) {
			//Assign a new write frontier block
			plane_record->Data_wf[stream_id] = plane_record->Get_a_free_block(stream_id, false);
			gc_and_wl_unit->Check_gc_required(plane_record->Get_free_block_pool_size(), page_address);
		}

		plane_record->Check_bookkeeping_correctness(page_address);
	}

	void Flash_Block_Manager::Allocate_block_and_page_in_plane_for_gc_write(const stream_id_type stream_id, NVM::FlashMemory::Physical_Page_Address& page_address)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Valid_pages_count++;
		plane_record->Free_pages_count--;		
		page_address.BlockID = plane_record->GC_wf[stream_id]->BlockID;
		page_address.PageID = plane_record->GC_wf[stream_id]->Current_page_write_index++;
		program_transaction_issued(page_address);

		
		//The current write frontier block is written to the end
		if (plane_record->GC_wf[stream_id]->Current_page_write_index == pages_no_per_block) {
			//Assign a new write frontier block
			plane_record->GC_wf[stream_id] = plane_record->Get_a_free_block(stream_id, false);
			gc_and_wl_unit->Check_gc_required(plane_record->Get_free_block_pool_size(), page_address);
		}
		plane_record->Check_bookkeeping_correctness(page_address);
	}
	
	void Flash_Block_Manager::Allocate_Pages_in_block_and_invalidate_remaining_for_preconditioning(const stream_id_type stream_id, const NVM::FlashMemory::Physical_Page_Address& plane_address, std::vector<NVM::FlashMemory::Physical_Page_Address>& page_addresses)
	{
		if(page_addresses.size() > pages_no_per_block) {
			PRINT_ERROR("Error while precondition a physical block: the size of the address list is larger than the pages_no_per_block!")
		}
			
		PlaneBookKeepingType *plane_record = &plane_manager[plane_address.ChannelID][plane_address.ChipID][plane_address.DieID][plane_address.PlaneID];
		if (plane_record->Data_wf[stream_id]->Current_page_write_index > 0) {
			PRINT_ERROR("Illegal operation: the Allocate_Pages_in_block_and_invalidate_remaining_for_preconditioning function should be executed for an erased block!")
		}

		//Assign physical addresses
		for (int i = 0; i < page_addresses.size(); i++) {
			plane_record->Valid_pages_count++;
			plane_record->Free_pages_count--;
			page_addresses[i].BlockID = plane_record->Data_wf[stream_id]->BlockID;
			page_addresses[i].PageID = plane_record->Data_wf[stream_id]->Current_page_write_index++;
			plane_record->Check_bookkeeping_correctness(page_addresses[i]);
		}

		//Invalidate the remaining pages in the block
		NVM::FlashMemory::Physical_Page_Address target_address(plane_address);
		while (plane_record->Data_wf[stream_id]->Current_page_write_index < pages_no_per_block) {
			plane_record->Free_pages_count--;
			target_address.BlockID = plane_record->Data_wf[stream_id]->BlockID;
			target_address.PageID = plane_record->Data_wf[stream_id]->Current_page_write_index++;
			Invalidate_page_in_block_for_preconditioning(stream_id, target_address);
			plane_record->Check_bookkeeping_correctness(plane_address);
		}

		//Update the write frontier
		plane_record->Data_wf[stream_id] = plane_record->Get_a_free_block(stream_id, false);
	}

	void Flash_Block_Manager::Allocate_block_and_page_in_plane_for_translation_write(const stream_id_type streamID, NVM::FlashMemory::Physical_Page_Address& page_address, bool is_for_gc)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Valid_pages_count++;
		plane_record->Free_pages_count--;
		page_address.BlockID = plane_record->Translation_wf[streamID]->BlockID;
		page_address.PageID = plane_record->Translation_wf[streamID]->Current_page_write_index++;
		program_transaction_issued(page_address);

		//The current write frontier block for translation pages is written to the end
		if (plane_record->Translation_wf[streamID]->Current_page_write_index == pages_no_per_block) {
			//Assign a new write frontier block
			plane_record->Translation_wf[streamID] = plane_record->Get_a_free_block(streamID, true);
			if (!is_for_gc) {
				gc_and_wl_unit->Check_gc_required(plane_record->Get_free_block_pool_size(), page_address);
			}
		}
		plane_record->Check_bookkeeping_correctness(page_address);
	}

	inline void Flash_Block_Manager::Invalidate_page_in_block(const stream_id_type stream_id, const NVM::FlashMemory::Physical_Page_Address& page_address)
	{
		PlaneBookKeepingType* plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Invalid_pages_count++;
		plane_record->Valid_pages_count--;
		if (plane_record->Blocks[page_address.BlockID].Stream_id != stream_id) {
			PRINT_ERROR("Inconsistent status in the Invalidate_page_in_block function! The accessed block is not allocated to stream " << stream_id)
		}
		plane_record->Blocks[page_address.BlockID].Invalid_page_count++;
		plane_record->Blocks[page_address.BlockID].Invalid_page_bitmap[page_address.PageID / 64] |= ((uint64_t)0x1) << (page_address.PageID % 64);
	}

	inline void Flash_Block_Manager::Invalidate_page_in_block_for_preconditioning(const stream_id_type stream_id, const NVM::FlashMemory::Physical_Page_Address& page_address)
	{
		PlaneBookKeepingType* plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Invalid_pages_count++;
		if (plane_record->Blocks[page_address.BlockID].Stream_id != stream_id) {
			PRINT_ERROR("Inconsistent status in the Invalidate_page_in_block function! The accessed block is not allocated to stream " << stream_id)
		}
		plane_record->Blocks[page_address.BlockID].Invalid_page_count++;
		plane_record->Blocks[page_address.BlockID].Invalid_page_bitmap[page_address.PageID / 64] |= ((uint64_t)0x1) << (page_address.PageID % 64);
	}

	void Flash_Block_Manager::Add_erased_block_to_pool(const NVM::FlashMemory::Physical_Page_Address& block_address)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[block_address.ChannelID][block_address.ChipID][block_address.DieID][block_address.PlaneID];
		Block_Pool_Slot_Type* block = &(plane_record->Blocks[block_address.BlockID]);
		const unsigned int invalid_pages = block->Invalid_page_count;
		const unsigned int written_pages = block->Current_page_write_index;
		const unsigned int valid_pages = written_pages >= invalid_pages ? written_pages - invalid_pages : 0;
		const unsigned int unwritten_pages = pages_no_per_block >= written_pages ? pages_no_per_block - written_pages : 0;
		block->Erase();

		if (block->Erase_count >= max_allowed_block_erase_count) {
			block->Is_bad = true;
			plane_record->Invalid_pages_count -= std::min(plane_record->Invalid_pages_count, invalid_pages);
			plane_record->Valid_pages_count -= std::min(plane_record->Valid_pages_count, valid_pages);
			plane_record->Free_pages_count -= std::min(plane_record->Free_pages_count, unwritten_pages);
			plane_record->Total_pages_count -= std::min(plane_record->Total_pages_count, pages_no_per_block);
			bad_block_count++;
			const uint64_t pending_retirements = Get_pending_retirement_count();
			const uint64_t committed_retirements = bad_block_count + pending_retirements;
			if (bad_block_count == 1) {
				PRINT_MESSAGE("[ENDURANCE] First bad block retired at P/E limit; reserve_blocks="
					<< op_spare_block_limit << " time=" << Simulator->Time())
			}
			if (!op_exhausted && op_spare_block_limit > 0 && committed_retirements >= op_spare_block_limit) {
				op_exhausted = true;
				op_exhaustion_time = Simulator->Time();
				PRINT_MESSAGE("[ENDURANCE] OP reserve exhausted: retired_bad_blocks=" << bad_block_count
					<< " pending_retirements=" << pending_retirements
					<< " committed_blocks=" << committed_retirements
					<< " reserve_blocks=" << op_spare_block_limit << " time=" << op_exhaustion_time)
				Simulator->Stop_simulation();
			}
			plane_record->Check_bookkeeping_correctness(block_address);
			return;
		}

		plane_record->Free_pages_count += invalid_pages;
		plane_record->Invalid_pages_count -= invalid_pages;
		plane_record->Add_to_free_block_pool(block, gc_and_wl_unit->Use_dynamic_wearleveling());
		plane_record->Check_bookkeeping_correctness(block_address);
	}

	inline unsigned int Flash_Block_Manager::Get_pool_size(const NVM::FlashMemory::Physical_Page_Address& plane_address)
	{
		return (unsigned int) plane_manager[plane_address.ChannelID][plane_address.ChipID][plane_address.DieID][plane_address.PlaneID].Free_block_pool.size();
	}
}
