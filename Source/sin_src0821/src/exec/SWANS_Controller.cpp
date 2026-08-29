#include <cmath>
#include <list>
#include <stdexcept>
#include "SWANS_Controller.h"
#include "../sim/Engine.h"
#include "../ssd/FTL.h"
#include "../ssd/Address_Mapping_Unit_Base.h"
#include "../ssd/Flash_Block_Manager_Base.h"
#include "../ssd/NVM_PHY_ONFI.h"
#include "../ssd/NVM_Transaction_Flash_RD.h"
#include "../ssd/NVM_Transaction_Flash_WR.h"

namespace SSD_Components
{
	SWANS_Controller::SWANS_Controller(const sim_object_id_type& id, SSD_Array_Policy_Type policy,
		unsigned int ssd_no_in_array, unsigned int zone_size_in_pages, unsigned int sectors_per_page,
		double th_precautionary_bytes, double th_critical_bytes,
		sim_time_type epoch_default, sim_time_type epoch_placement, sim_time_type epoch_migration,
		Caching_Mode* caching_mode_per_input_stream, unsigned int stream_count,
		unsigned int channel_no_per_ssd, unsigned int chip_no_per_channel, unsigned int die_no_per_chip,
		unsigned int plane_no_per_die, unsigned int block_no_per_plane) :
		Data_Cache_Manager_Base(id, NULL, NULL, 8192, 800, 4, 13, 13, 13, caching_mode_per_input_stream, Cache_Sharing_Mode::SHARED, stream_count),
		policy(policy),
		ssd_no_in_array(ssd_no_in_array), zone_size_in_pages(zone_size_in_pages), sectors_per_page(sectors_per_page),
		th_precautionary_bytes(th_precautionary_bytes), th_critical_bytes(th_critical_bytes),
		epoch_default(epoch_default), epoch_placement(epoch_placement), epoch_migration(epoch_migration),
		channel_no_per_ssd(channel_no_per_ssd), chip_no_per_channel(chip_no_per_channel), die_no_per_chip(die_no_per_chip),
		plane_no_per_die(plane_no_per_die), block_no_per_plane(block_no_per_plane)
	{
		if (ssd_no_in_array < 2) {
			PRINT_ERROR("SWANS: the number of SSDs in the array must be at least 2!")
		}
		if (zone_size_in_pages == 0) {
			PRINT_ERROR("SWANS: the zone size is too small!")
		}

		page_size_in_bytes = sectors_per_page * SECTOR_SIZE_IN_BYTE;
		if (sectors_per_page >= 64) {
			full_page_bitmap = 0xffffffffffffffffULL;
		} else {
			full_page_bitmap = ~(0xffffffffffffffffULL << sectors_per_page);
		}

		ssd_info_table.resize(ssd_no_in_array);
		for (unsigned int i = 0; i < ssd_no_in_array; i++) {
			ssd_info_table[i].Number_of_writes = 0;
			ssd_info_table[i].Balance_bytes = 0;
			ssd_info_table[i].TBW_bytes = 0;
			ssd_info_table[i].Redirected_writes = 0;
		}

		placement_active = false;
		t_next_test = epoch_default;
		migration.Active = false;
		migration.Pending_writes = 0;

		stat_data_placement_processes = 0;
		stat_redirected_write_transactions = 0;
		stat_data_migration_processes = 0;
		stat_migrated_pages = 0;
		stat_mu_tests = 0;
		stat_parent_requests = 0;
		stat_child_requests = 0;
	}

	SWANS_Controller::~SWANS_Controller()
	{
		//The member SSD components are owned and deleted by SSD_Device
	}

	void SWANS_Controller::Add_member_ssd(FTL* ftl, Data_Cache_Manager_Base* dcm, NVM_PHY_ONFI* phy)
	{
		Member_SSD member;
		member.Ftl = ftl;
		member.Dcm = dcm;
		member.Phy = phy;
		members.push_back(member);
	}

	//Builds the Zone Info Table of each I/O stream. The initial mapping stripes the logical zones over
	//the member SSDs in a round-robin manner (i.e., logical zone z is placed on SSD (z % N)), which
	//realizes the RAID-0 data distribution of the array at zone granularity.
	void SWANS_Controller::initialize_zoning()
	{
		streams.resize(stream_count);
		for (unsigned int s = 0; s < stream_count; s++) {
			Stream_Zoning& sz = streams[s];
			sz.Member_pages = members[0].Ftl->Address_Mapping_Unit->Get_logical_pages_count(s);
			sz.Zones_per_ssd = sz.Member_pages / zone_size_in_pages;
			sz.Zone_count = sz.Zones_per_ssd * ssd_no_in_array;
			if (sz.Zone_count == 0) {
				PRINT_ERROR("SWANS: the logical space of I/O flow #" << s << " is smaller than one zone per SSD! Please decrease the zone size.")
			}
			sz.Zone_info_table.resize(sz.Zone_count);
			sz.Empty_zone_set.resize(ssd_no_in_array);
			for (uint64_t z = 0; z < sz.Zone_count; z++) {
				sz.Zone_info_table[z].Physical_SSD_number = (unsigned int)(z % ssd_no_in_array);
				sz.Zone_info_table[z].Physical_zone_number = (unsigned int)(z / ssd_no_in_array);
				sz.Zone_info_table[z].Number_of_writes = 0;
				sz.Zone_info_table[z].Written_bytes = 0;
				sz.Empty_zone_set[z % ssd_no_in_array].insert(z);
			}
		}
	}

	void SWANS_Controller::Start_simulation()
	{
		if (streams.size() == 0) {
			initialize_zoning();
		}
		t_next_test = Simulator->Time() + epoch_default;
	}

	void SWANS_Controller::Validate_simulation_config()
	{
		if (members.size() != ssd_no_in_array) {
			PRINT_ERROR("SWANS: the number of registered member SSDs does not match SSD_No_In_Array!")
		}
		if (host_interface == NULL) {
			PRINT_ERROR("SWANS: the array controller is not connected to a host interface!")
		}
	}

	void SWANS_Controller::Execute_simulator_event(MQSimEngine::Sim_Event* event) {}

	void SWANS_Controller::Do_warmup(std::vector<Utils::Workload_Statistics*> workload_stats) {}

	void SWANS_Controller::Setup_triggers()
	{
		Data_Cache_Manager_Base::Setup_triggers();//Connects this controller to the host interface's request-arrival signal
		SWANS_Controller* ctrl = this;
		for (unsigned int m = 0; m < members.size(); m++) {
			//Completion path of the RAID-0 child requests
			members[m].Dcm->Connect_to_user_request_serviced_signal([ctrl](User_Request* child) { ctrl->handle_child_request_serviced(child); });
			//Per-transaction statistics of the host interface: forward the member SSDs' signals upwards
			members[m].Dcm->Connect_to_user_memory_transaction_serviced_signal([ctrl](NVM_Transaction* transaction) { ctrl->broadcast_user_memory_transaction_serviced_signal(transaction); });
			//Completion path of the SWANS zone-migration transactions
			members[m].Phy->ConnectToTransactionServicedSignal([ctrl](NVM_Transaction_Flash* transaction) { ctrl->handle_member_phy_transaction_serviced(transaction); });
		}
	}

	unsigned int SWANS_Controller::hottest_ssd() const
	{
		unsigned int hottest = 0;
		for (unsigned int i = 1; i < ssd_no_in_array; i++) {
			if (ssd_info_table[i].Balance_bytes > ssd_info_table[hottest].Balance_bytes) {
				hottest = i;
			}
		}
		return hottest;
	}

	unsigned int SWANS_Controller::coldest_ssd() const
	{
		unsigned int coldest = 0;
		for (unsigned int i = 1; i < ssd_no_in_array; i++) {
			if (ssd_info_table[i].Balance_bytes < ssd_info_table[coldest].Balance_bytes) {
				coldest = i;
			}
		}
		return coldest;
	}

	//Swaps the physical locations (SSD number and physical zone number) of two logical zones
	//(the paper's SwapPhysicalZone and SwapPhysicalSSD operations) and maintains the empty-zone sets.
	void SWANS_Controller::swap_zones(stream_id_type stream_id, uint64_t zone_a, uint64_t zone_b)
	{
		Stream_Zoning& sz = streams[stream_id];
		Zone_Info& za = sz.Zone_info_table[zone_a];
		Zone_Info& zb = sz.Zone_info_table[zone_b];

		if (za.Number_of_writes == 0) {
			sz.Empty_zone_set[za.Physical_SSD_number].erase(zone_a);
			sz.Empty_zone_set[zb.Physical_SSD_number].insert(zone_a);
		}
		if (zb.Number_of_writes == 0) {
			sz.Empty_zone_set[zb.Physical_SSD_number].erase(zone_b);
			sz.Empty_zone_set[za.Physical_SSD_number].insert(zone_b);
		}

		unsigned int tmp_ssd = za.Physical_SSD_number;
		unsigned int tmp_zone = za.Physical_zone_number;
		za.Physical_SSD_number = zb.Physical_SSD_number;
		za.Physical_zone_number = zb.Physical_zone_number;
		zb.Physical_SSD_number = tmp_ssd;
		zb.Physical_zone_number = tmp_zone;
	}

	void SWANS_Controller::map_read(stream_id_type stream_id, LPA_type lpa, unsigned int& member, LPA_type& member_lpa)
	{
		Stream_Zoning& sz = streams[stream_id];
		uint64_t zone = lpa / zone_size_in_pages;
		if (zone >= sz.Zone_count) {
			//The small tail of the array space that is not covered by zones is statically striped
			LPA_type leftover = lpa - sz.Zone_count * zone_size_in_pages;
			member = (unsigned int)(leftover % ssd_no_in_array);
			member_lpa = sz.Zones_per_ssd * zone_size_in_pages + leftover / ssd_no_in_array;
			if (member_lpa >= sz.Member_pages) {
				member_lpa = sz.Member_pages - 1;
			}
			return;
		}
		Zone_Info& zi = sz.Zone_info_table[zone];
		member = zi.Physical_SSD_number;
		member_lpa = (LPA_type)zi.Physical_zone_number * zone_size_in_pages + lpa % zone_size_in_pages;
	}

	//WAM + DPM for a single page-write transaction (Algorithms 1 and 2 of the paper)
	void SWANS_Controller::map_write(stream_id_type stream_id, LPA_type lpa, unsigned int transferred_bytes, unsigned int& member, LPA_type& member_lpa)
	{
		Stream_Zoning& sz = streams[stream_id];
		uint64_t zone = lpa / zone_size_in_pages;
		if (zone >= sz.Zone_count) {
			map_read(stream_id, lpa, member, member_lpa);//statically striped tail
			ssd_info_table[member].Number_of_writes++;
			ssd_info_table[member].Balance_bytes += transferred_bytes;
			ssd_info_table[member].TBW_bytes += transferred_bytes;
			return;
		}
		uint64_t offset = lpa % zone_size_in_pages;
		Zone_Info* zi = &sz.Zone_info_table[zone];

		//DPM (SWANS policy only): while a data placement process is active, a write that targets an
		//empty zone of the hottest SSD is redirected to the coldest SSD by exchanging the physical
		//locations of the target zone and an empty zone of the coldest SSD (no data movement is needed).
		//Under the plain RAID0 policy, placement_active is never set, so writes always follow the
		//static round-robin zone striping.
		if (placement_active && zi->Number_of_writes == 0 && zi->Physical_SSD_number == hottest_ssd()) {
			unsigned int coldest = coldest_ssd();
			if (coldest != zi->Physical_SSD_number && !sz.Empty_zone_set[coldest].empty()) {
				//Do not hand out the empty zone that an in-flight DMH migration has already reserved as
				//its destination (see Migration_Info::Cold_zone) -- its physical location is not actually
				//free until that migration finishes.
				bool reserved_by_migration = migration.Active && stream_id == migration.Stream_id;
				uint64_t empty_zone = 0;
				bool found = false;
				for (uint64_t candidate : sz.Empty_zone_set[coldest]) {
					if (reserved_by_migration && candidate == migration.Cold_zone) {
						continue;
					}
					empty_zone = candidate;
					found = true;
					break;
				}
				if (found) {
					swap_zones(stream_id, zone, empty_zone);
					stat_data_placement_processes++;
				}
			}
		}
		if (placement_active && zi->Number_of_writes == 0 && zi->Physical_SSD_number != (unsigned int)(zone % ssd_no_in_array)) {
			stat_redirected_write_transactions++;
			ssd_info_table[zi->Physical_SSD_number].Redirected_writes++;
		}

		//WAM: update the popularity of the zone (ZIT) and of its SSD (SIT). The statistics are also
		//maintained under the plain RAID0 policy so that the two policies can be compared directly.
		if (zi->Number_of_writes == 0) {
			sz.Empty_zone_set[zi->Physical_SSD_number].erase(zone);//the zone becomes a used zone
			if (policy == SSD_Array_Policy_Type::SWANS) {
				zi->Page_write_map.resize((zone_size_in_pages + 63) / 64, 0);//only needed for zone migration
			}
		}
		zi->Number_of_writes++;
		zi->Written_bytes += transferred_bytes;
		if (policy == SSD_Array_Policy_Type::SWANS) {
			zi->Page_write_map[offset / 64] |= (1ULL << (offset % 64));
		}
		ssd_info_table[zi->Physical_SSD_number].Number_of_writes++;
		ssd_info_table[zi->Physical_SSD_number].Balance_bytes += transferred_bytes;
		ssd_info_table[zi->Physical_SSD_number].TBW_bytes += transferred_bytes;

		member = zi->Physical_SSD_number;
		member_lpa = (LPA_type)zi->Physical_zone_number * zone_size_in_pages + offset;
	}

	//RAID-0 request distribution: translates the LPAs of the host request's transactions, groups the
	//transactions per member SSD, and forwards one child request to each involved member SSD.
	void SWANS_Controller::process_new_user_request(User_Request* user_request)
	{
		if (user_request->Transaction_list.size() == 0) {
			return;
		}

		//While a DMH migration is in flight, hold back any request that touches the zone being migrated
		//(the source, still logically the hot zone) or the zone it is migrating into (the destination,
		//still logically the empty/cold zone) until the migration fully completes and the zone mapping is
		//swapped (see handle_member_phy_transaction_serviced). This is the paper's treatment of a
		//read/write that arrives for a zone whose physical location is currently mid-move: it is queued,
		//not serviced against a stale or half-moved location.
		if (migration.Active) {
			for (auto &tr : user_request->Transaction_list) {
				NVM_Transaction_Flash* transaction = (NVM_Transaction_Flash*)tr;
				if (transaction->Stream_id != migration.Stream_id) {
					continue;
				}
				Stream_Zoning& sz = streams[transaction->Stream_id];
				uint64_t zone = transaction->LPA / zone_size_in_pages;
				if (zone < sz.Zone_count && (zone == migration.Migrating_zone || zone == migration.Cold_zone)) {
					deferred_requests.push_back(user_request);
					return;
				}
			}
		}

		stat_parent_requests++;

		std::vector<std::list<NVM_Transaction*>> groups(ssd_no_in_array);
		for (auto &tr : user_request->Transaction_list) {
			NVM_Transaction_Flash* transaction = (NVM_Transaction_Flash*)tr;
			unsigned int member;
			LPA_type member_lpa;
			if (user_request->Type == UserRequestType::WRITE) {
				map_write(transaction->Stream_id, transaction->LPA, transaction->Data_and_metadata_size_in_byte, member, member_lpa);
			} else {
				map_read(transaction->Stream_id, transaction->LPA, member, member_lpa);
			}
			transaction->LPA = member_lpa;
			groups[member].push_back(tr);
		}
		user_request->Transaction_list.clear();

		unsigned int child_count = 0;
		for (unsigned int m = 0; m < ssd_no_in_array; m++) {
			if (groups[m].size() > 0) {
				child_count++;
			}
		}
		pending_children[user_request] = child_count;

		for (unsigned int m = 0; m < ssd_no_in_array; m++) {
			if (groups[m].size() == 0) {
				continue;
			}
			User_Request* child = new User_Request;
			child->Type = user_request->Type;
			child->Stream_id = user_request->Stream_id;
			child->Priority_class = user_request->Priority_class;
			child->STAT_InitiationTime = user_request->STAT_InitiationTime;
			child->Start_LBA = ((NVM_Transaction_Flash*)groups[m].front())->LPA * sectors_per_page;
			unsigned int size_in_byte = 0;
			for (auto &tr : groups[m]) {
				tr->UserIORequest = child;
				child->Transaction_list.push_back(tr);
				size_in_byte += ((NVM_Transaction_Flash*)tr)->Data_and_metadata_size_in_byte;
			}
			child->Size_in_byte = size_in_byte;
			child->SizeInSectors = size_in_byte / SECTOR_SIZE_IN_BYTE;
			child_to_parent[child] = user_request;
			stat_child_requests++;
			members[m].Dcm->Receive_user_request(child);
		}

		//Algorithm 1 (SWANS policy only): on the arrival of a request, check if it is time to test mu.
		//The plain RAID0 policy performs no array-level wear leveling.
		if (policy == SSD_Array_Policy_Type::SWANS && Simulator->Time() >= t_next_test) {
			test_write_distribution();
		}
	}

	//When all child requests of a parent host request are serviced, the parent is completed
	//towards the host interface.
	void SWANS_Controller::handle_child_request_serviced(User_Request* child)
	{
		auto it = child_to_parent.find(child);
		if (it == child_to_parent.end()) {
			return;
		}
		User_Request* parent = it->second;
		child_to_parent.erase(it);
		delete child;//the child's transaction list is already empty (checked by the member SSD's cache manager)

		auto pending = pending_children.find(parent);
		pending->second--;
		if (pending->second == 0) {
			pending_children.erase(pending);
			broadcast_user_request_serviced_signal(parent);//goes to the host interface
		}
	}

	double SWANS_Controller::compute_mu_bytes() const
	{
		double mean = 0;
		for (unsigned int i = 0; i < ssd_no_in_array; i++) {
			mean += (double)ssd_info_table[i].Balance_bytes;
		}
		mean /= ssd_no_in_array;
		double var = 0;
		for (unsigned int i = 0; i < ssd_no_in_array; i++) {
			double diff = (double)ssd_info_table[i].Balance_bytes - mean;
			var += diff * diff;
		}
		return std::sqrt(var / ssd_no_in_array);
	}

	//The periodic test of the WAM module: measures mu (the standard deviation of the written data
	//across member SSDs) and decides which data organization method should be activated.
	void SWANS_Controller::test_write_distribution()
	{
		stat_mu_tests++;
		double mu = compute_mu_bytes();
		if (mu < th_precautionary_bytes) {
			placement_active = false;
			t_next_test = Simulator->Time() + epoch_default;
		} else if (mu < th_critical_bytes) {
			placement_active = true;//launch a data placement process (DPM)
			t_next_test = Simulator->Time() + epoch_placement;
		} else {
			placement_active = false;
			t_next_test = Simulator->Time() + epoch_migration;
			if (!migration.Active) {
				start_data_migration();//launch a data migration process (DMH)
			}
		}
	}

	//The DMH module (Algorithm 3): migrates the most popular zone of the hottest SSD to an empty
	//zone of the coldest SSD. The data movement is simulated by issuing flash read transactions on
	//the hot member SSD followed by write transactions on the cold one; the zone mapping itself is
	//swapped only once that data movement fully completes (see handle_member_phy_transaction_serviced).
	void SWANS_Controller::start_data_migration()
	{
		unsigned int hot_ssd = hottest_ssd();
		unsigned int cold_ssd = coldest_ssd();
		if (hot_ssd == cold_ssd) {
			return;
		}

		//Find the most popular (hottest) zone residing on the hottest SSD
		stream_id_type hot_stream = 0;
		uint64_t hot_zone = 0;
		uint64_t max_writes = 0;
		for (unsigned int s = 0; s < stream_count; s++) {
			for (uint64_t z = 0; z < streams[s].Zone_count; z++) {
				Zone_Info& zi = streams[s].Zone_info_table[z];
				if (zi.Physical_SSD_number == hot_ssd && zi.Number_of_writes > max_writes) {
					max_writes = zi.Number_of_writes;
					hot_stream = s;
					hot_zone = z;
				}
			}
		}
		if (max_writes == 0) {
			return;//no used zone on the hottest SSD
		}

		Stream_Zoning& sz = streams[hot_stream];
		if (sz.Empty_zone_set[cold_ssd].empty()) {
			return;//no empty zone available on the coldest SSD
		}
		uint64_t cold_zone = *(sz.Empty_zone_set[cold_ssd].begin());

		Zone_Info& hot_zi = sz.Zone_info_table[hot_zone];
		Zone_Info& cold_zi = sz.Zone_info_table[cold_zone];

		//If this member SSD's own internal GC/wear-leveling is concurrently relocating any page of the
		//chosen zone (for its own, independent reasons), postpone the whole migration to a later mu
		//test. Migrating some offsets while leaving a GC-locked one in place would swap the zone's
		//physical location for ALL offsets (including the skipped one), so a partial migration would
		//silently strand that offset's data on the old (hot) SSD while the array believes it now lives
		//on the cold SSD -- a data-correctness gap. Waiting for the lock to clear avoids that entirely.
		for (uint64_t offset = 0; offset < zone_size_in_pages; offset++) {
			if (hot_zi.Page_write_map[offset / 64] & (1ULL << (offset % 64))) {
				LPA_type source_lpa = (LPA_type)hot_zi.Physical_zone_number * zone_size_in_pages + offset;
				if (members[hot_ssd].Ftl->Address_Mapping_Unit->Is_lpa_locked_for_gc(hot_stream, source_lpa)) {
					return;
				}
			}
		}

		//Prepare the migration I/O: only the pages that have been written participate in the migration
		migration.Active = true;
		migration.Stream_id = hot_stream;
		migration.Hot_SSD = hot_ssd;
		migration.Cold_SSD = cold_ssd;
		migration.Migrating_zone = hot_zone;
		migration.Read_to_write_lpa.clear();
		std::list<NVM_Transaction*> read_transactions;
		for (uint64_t offset = 0; offset < zone_size_in_pages; offset++) {
			if (hot_zi.Page_write_map[offset / 64] & (1ULL << (offset % 64))) {
				LPA_type source_lpa = (LPA_type)hot_zi.Physical_zone_number * zone_size_in_pages + offset;
				LPA_type dest_lpa = (LPA_type)cold_zi.Physical_zone_number * zone_size_in_pages + offset;
				migration.Read_to_write_lpa[source_lpa] = dest_lpa;
				read_transactions.push_back(new NVM_Transaction_Flash_RD(Transaction_Source_Type::SWANS, hot_stream,
					page_size_in_bytes, source_lpa, NO_PPA, NULL, 0, NULL, full_page_bitmap, CurrentTimeStamp));
			}
		}
		migration.Pending_writes = (unsigned int)migration.Read_to_write_lpa.size();
		if (migration.Pending_writes == 0) {
			migration.Active = false;
			return;
		}
		migration.Cold_zone = cold_zone;

		stat_data_migration_processes++;
		stat_migrated_pages += migration.Pending_writes;

		//ZoneGrabber: read the written pages of the hottest zone from the hot member SSD. The logical zone
		//mapping (SwapPhysicalZone + SwapPhysicalSSD) and the SIT balance update (SwapZoneStats +
		//UpdateDiskStats) are deliberately NOT performed yet -- see handle_member_phy_transaction_serviced,
		//where they happen only once every page has actually been copied out and invalidated on the hot
		//member. Swapping the mapping immediately would let a brand-new write to the (still logically
		//empty) cold zone alias onto the hot zone's physical territory while the migration is still
		//reading it out, racing the migration's own invalidation of that territory and corrupting the hot
		//member's mapping table.
		members[hot_ssd].Ftl->Address_Mapping_Unit->Translate_lpa_to_ppa_and_dispatch(read_transactions);
	}

	//ZoneRestorer: when a migration read completes on the hot member SSD, the page is written to the
	//destination zone on the cold member SSD; when all migration writes complete, the data migration
	//process terminates.
	void SWANS_Controller::handle_member_phy_transaction_serviced(NVM_Transaction_Flash* transaction)
	{
		if (transaction->Source != Transaction_Source_Type::SWANS) {
			return;
		}

		if (transaction->Type == Transaction_Type::READ) {
			auto it = migration.Read_to_write_lpa.find(transaction->LPA);
			if (it == migration.Read_to_write_lpa.end()) {
				return;
			}
			LPA_type dest_lpa = it->second;
			LPA_type source_lpa = transaction->LPA;
			migration.Read_to_write_lpa.erase(it);
			std::list<NVM_Transaction*> write_transactions;
			write_transactions.push_back(new NVM_Transaction_Flash_WR(Transaction_Source_Type::SWANS, migration.Stream_id,
				page_size_in_bytes, dest_lpa, NO_PPA, NULL, 0, NULL, full_page_bitmap, CurrentTimeStamp));
			ssd_info_table[migration.Cold_SSD].TBW_bytes += page_size_in_bytes;
			members[migration.Cold_SSD].Ftl->Address_Mapping_Unit->Translate_lpa_to_ppa_and_dispatch(write_transactions);
			//The read only copies the data out; it never touches the hot member's own mapping table.
			//Without this, the hot member's FTL would still believe source_lpa's old page is valid
			//forever (nothing else ever invalidates it), while SWANS's zone bookkeeping now treats that
			//same physical territory as free for reuse -- the two views would silently diverge and
			//eventually make the hot member's own GC find a "valid" page whose metadata no longer
			//matches the mapping table (see Address_Mapping_Unit_Page_Level::Set_barrier_for_accessing_physical_block).
			members[migration.Hot_SSD].Ftl->Address_Mapping_Unit->Invalidate_lpa_mapping(migration.Stream_id, source_lpa);
		} else if (transaction->Type == Transaction_Type::WRITE) {
			if (migration.Pending_writes > 0) {
				migration.Pending_writes--;
				if (migration.Pending_writes == 0) {
					migration.Active = false;
					//SwapZoneStats + UpdateDiskStats + SwapPhysicalZone + SwapPhysicalSSD: only now, after
					//every page has been copied out of the hot zone and invalidated on the hot member, is it
					//safe to hand the hot zone's physical territory to the (still logically empty) cold zone
					//-- nothing could have aliased onto it in the meantime, since no logical zone pointed
					//there until this instant.
					Stream_Zoning& sz = streams[migration.Stream_id];
					Zone_Info& hot_zi = sz.Zone_info_table[migration.Migrating_zone];
					ssd_info_table[migration.Hot_SSD].Balance_bytes -= hot_zi.Written_bytes;
					ssd_info_table[migration.Cold_SSD].Balance_bytes += hot_zi.Written_bytes;
					swap_zones(migration.Stream_id, migration.Migrating_zone, migration.Cold_zone);

					//Now that the zone mapping reflects the migration's outcome, re-submit any requests
					//that arrived while it was in flight and were held back because they targeted the
					//zone being migrated (source or destination). FIFO order is preserved.
					if (!deferred_requests.empty()) {
						std::list<User_Request*> ready;
						ready.swap(deferred_requests);
						for (User_Request* req : ready) {
							process_new_user_request(req);
						}
					}
				}
			}
		}
	}

	void SWANS_Controller::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter)
	{
		std::string tmp = name_prefix + ".SSD_Array";
		xmlwriter.Write_open_tag(tmp);

		std::string attr = "Policy";
		std::string val = (policy == SSD_Array_Policy_Type::SWANS ? "SWANS" : "RAID0");
		xmlwriter.Write_attribute_string(attr, val);

		attr = "SSD_No_In_Array";
		val = std::to_string(ssd_no_in_array);
		xmlwriter.Write_attribute_string(attr, val);

		attr = "Zone_Size_Pages";
		val = std::to_string(zone_size_in_pages);
		xmlwriter.Write_attribute_string(attr, val);

		attr = "Host_Requests";
		val = std::to_string(stat_parent_requests);
		xmlwriter.Write_attribute_string(attr, val);

		attr = "RAID0_Sub_Requests";
		val = std::to_string(stat_child_requests);
		xmlwriter.Write_attribute_string(attr, val);

		attr = "Final_Mu_SDW_MB";
		val = std::to_string(compute_mu_bytes() / (1024 * 1024));
		xmlwriter.Write_attribute_string(attr, val);

		//SDW of the physical TBW across SSDs
		{
			double mean = 0;
			for (unsigned int i = 0; i < ssd_no_in_array; i++) {
				mean += (double)ssd_info_table[i].TBW_bytes;
			}
			mean /= ssd_no_in_array;
			double var = 0;
			for (unsigned int i = 0; i < ssd_no_in_array; i++) {
				double diff = (double)ssd_info_table[i].TBW_bytes - mean;
				var += diff * diff;
			}
			attr = "Final_SD_of_TBW_MB";
			val = std::to_string(std::sqrt(var / ssd_no_in_array) / (1024 * 1024));
			xmlwriter.Write_attribute_string(attr, val);
		}

		attr = "Mu_Test_Count";
		val = std::to_string(stat_mu_tests);
		xmlwriter.Write_attribute_string(attr, val);

		attr = "Data_Placement_Zone_Swaps";
		val = std::to_string(stat_data_placement_processes);
		xmlwriter.Write_attribute_string(attr, val);

		attr = "Redirected_Write_Transactions";
		val = std::to_string(stat_redirected_write_transactions);
		xmlwriter.Write_attribute_string(attr, val);

		attr = "Data_Migration_Processes";
		val = std::to_string(stat_data_migration_processes);
		xmlwriter.Write_attribute_string(attr, val);

		attr = "Migrated_Pages";
		val = std::to_string(stat_migrated_pages);
		xmlwriter.Write_attribute_string(attr, val);

		//Per member SSD statistics: writes, TBW, and flash wear (block erase counts of the member SSD)
		for (unsigned int ssd = 0; ssd < ssd_no_in_array; ssd++) {
			std::string ssd_tag = "SSD_" + std::to_string(ssd);
			xmlwriter.Write_open_tag(ssd_tag);

			attr = "Number_of_Page_Writes";
			val = std::to_string(ssd_info_table[ssd].Number_of_writes);
			xmlwriter.Write_attribute_string(attr, val);

			attr = "TBW_MB";
			val = std::to_string((double)ssd_info_table[ssd].TBW_bytes / (1024 * 1024));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Balance_Write_MB";
			val = std::to_string((double)ssd_info_table[ssd].Balance_bytes / (1024 * 1024));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Redirected_Write_Transactions";
			val = std::to_string(ssd_info_table[ssd].Redirected_writes);
			xmlwriter.Write_attribute_string(attr, val);

			//Sum the block erase counts of this member SSD (its own block manager keeps the wear state)
			uint64_t total_erases = 0;
			unsigned int max_block_erase = 0;
			NVM::FlashMemory::Physical_Page_Address addr;
			for (unsigned int ch = 0; ch < channel_no_per_ssd; ch++) {
				for (unsigned int chip = 0; chip < chip_no_per_channel; chip++) {
					for (unsigned int die = 0; die < die_no_per_chip; die++) {
						for (unsigned int plane = 0; plane < plane_no_per_die; plane++) {
							addr.ChannelID = ch;
							addr.ChipID = chip;
							addr.DieID = die;
							addr.PlaneID = plane;
							PlaneBookKeepingType* pbke = members[ssd].Ftl->BlockManager->Get_plane_bookkeeping_entry(addr);
							for (unsigned int block = 0; block < block_no_per_plane; block++) {
								total_erases += pbke->Blocks[block].Erase_count;
								if (pbke->Blocks[block].Erase_count > max_block_erase) {
									max_block_erase = pbke->Blocks[block].Erase_count;
								}
							}
						}
					}
				}
			}

			attr = "Total_Block_Erase_Count";
			val = std::to_string(total_erases);
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Max_Block_Erase_Count";
			val = std::to_string(max_block_erase);
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Bad_Block_Count";
			val = std::to_string(members[ssd].Ftl->BlockManager->Get_bad_block_count());
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Current_OP_Ratio";
			val = std::to_string(members[ssd].Ftl->BlockManager->Get_current_op_ratio());
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Reached_End_Of_Life";
			val = members[ssd].Ftl->BlockManager->Has_reached_end_of_life() ? "true" : "false";
			xmlwriter.Write_attribute_string(attr, val);

			xmlwriter.Write_close_tag();
		}

		xmlwriter.Write_close_tag();
	}
}
