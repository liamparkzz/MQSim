#ifndef SWANS_CONTROLLER_H
#define SWANS_CONTROLLER_H

#include <vector>
#include <list>
#include <unordered_set>
#include <unordered_map>
#include <cstdint>
#include "../sim/Sim_Defs.h"
#include "../sim/Sim_Object.h"
#include "../ssd/SSD_Defs.h"
#include "../ssd/User_Request.h"
#include "../ssd/NVM_Transaction_Flash.h"
#include "../ssd/Data_Cache_Manager_Base.h"
#include "SWANS_Parameter_Set.h"

/*********************************************************************************************************
* SWANS_Controller: the RAID-0 SSD-array controller with the SWANS interdisk wear-leveling strategy.
*
* Reference: W. Wang, T. Xie, and A. Sharma, "SWANS: An Interdisk Wear-Leveling Strategy for
* RAID-0 Structured SSD Arrays", ACM Transactions on Storage, Vol. 12, No. 3, 2016.
*
* Architecture
* ------------
* The RAID-0 array is built out of N *complete and independent* member SSD instances. Each member
* SSD owns a full MQSim SSD back-end: its own data cache manager, FTL, address mapping unit,
* GC/wear-leveling unit, transaction scheduling unit, PHY, flash channels, and flash chips
* (see SSD_Device.cpp, which instantiates the N back-ends). The array controller is implemented
* as a Data_Cache_Manager_Base subclass, so the (single) host interface hands every arriving host
* request to the array controller exactly like it would hand it to a plain SSD cache manager.
*
* For every host request, the controller:
*   1. translates each page-sized transaction's array-level LPA into a (member SSD, member LPA)
*      pair according to the current zone map (RAID-0 zone striping + SWANS decisions),
*   2. packages the transactions of each target member SSD into a child User_Request and forwards
*      it to that member's own cache manager (Data_Cache_Manager_Base::Receive_user_request), and
*   3. completes the parent host request towards the host interface when all children complete.
*
* The three SWANS modules are implemented on top of this array controller:
*   - WAM (Write Access Monitor, Algorithm 1): updates the Zone Info Table and SSD Info Table on
*     every write and periodically tests mu, the standard deviation of written data across the
*     member SSDs, against th_precautionary and th_critical.
*   - DPM (Data Placement Manager, Algorithm 2): while a data placement process is active, a write
*     that targets an empty zone of the hottest SSD is redirected by swapping the zone's physical
*     location with an empty zone of the coldest SSD (no data movement).
*   - DMH (Data Migration Handler, Algorithm 3): when mu >= th_critical, the most popular zone of
*     the hottest SSD is migrated to an empty zone of the coldest SSD by issuing real flash read
*     transactions on the hot member and write transactions on the cold member
*     (Transaction_Source_Type::SWANS), so the timing overhead of migration is fully simulated.
*
* The logical address space of the array is divided into fixed-size zones (paper default: 16MB).
* The initial mapping stripes the logical zones round-robin over the member SSDs, which realizes
* the RAID-0 data distribution at zone granularity.
*
* Array policy
* ------------
* The controller supports two data distribution policies (SSD_Array_Policy_Type):
*   - RAID0: the plain RAID-0 baseline of the SWANS paper. The zone map stays at its static
*     round-robin striping and no array-level wear leveling is performed (no mu tests, no write
*     redirection, no zone migration). The per-SSD write statistics are still collected, so the
*     results are directly comparable with the SWANS policy.
*   - SWANS: RAID-0 plus the WAM/DPM/DMH modules described above.
*********************************************************************************************************/

namespace SSD_Components
{
	class FTL;
	class NVM_PHY_ONFI;

	class SWANS_Controller : public Data_Cache_Manager_Base
	{
	public:
		SWANS_Controller(const sim_object_id_type& id, SSD_Array_Policy_Type policy,
			unsigned int ssd_no_in_array, unsigned int zone_size_in_pages, unsigned int sectors_per_page,
			double th_precautionary_bytes, double th_critical_bytes,
			sim_time_type epoch_default, sim_time_type epoch_placement, sim_time_type epoch_migration,
			Caching_Mode* caching_mode_per_input_stream, unsigned int stream_count,
			unsigned int channel_no_per_ssd, unsigned int chip_no_per_channel, unsigned int die_no_per_chip,
			unsigned int plane_no_per_die, unsigned int block_no_per_plane);
		~SWANS_Controller();

		//Registers one member SSD (a complete SSD back-end instance) with the array controller.
		//Must be called once per member, in the order of the member SSD numbers (0 .. N-1).
		void Add_member_ssd(FTL* ftl, Data_Cache_Manager_Base* dcm, NVM_PHY_ONFI* phy);

		void Start_simulation();
		void Validate_simulation_config();
		void Execute_simulator_event(MQSimEngine::Sim_Event* event);
		void Setup_triggers();
		void Do_warmup(std::vector<Utils::Workload_Statistics*> workload_stats);
		void Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter);

	protected:
		//Receives a host request from the host interface, applies WAM/DPM, translates the array-level
		//LPAs, and distributes the request over the member SSDs (RAID-0)
		void process_new_user_request(User_Request* user_request);

	private:
		//One member SSD of the array: a complete, independent MQSim SSD back-end
		struct Member_SSD
		{
			FTL* Ftl;
			Data_Cache_Manager_Base* Dcm;
			NVM_PHY_ONFI* Phy;
		};

		//An entry of the Zone Info Table (ZIT): the current physical location and write popularity of a logical zone
		struct Zone_Info
		{
			unsigned int Physical_SSD_number;
			unsigned int Physical_zone_number;
			uint64_t Number_of_writes;//number of page-write transactions received by this logical zone
			uint64_t Written_bytes;
			std::vector<uint64_t> Page_write_map;//bitmap of the zone pages that have been written at least once (the paper's BlockWriteMap at page granularity); allocated lazily on the first write
		};

		//An entry of the SSD Info Table (SIT): per member SSD statistics
		struct SSD_Info
		{
			uint64_t Number_of_writes;//number of page-write transactions routed to this SSD
			uint64_t Balance_bytes;//written bytes used by WAM to measure mu; migrated zones transfer their popularity to the destination SSD (the paper's UpdateDiskStats)
			uint64_t TBW_bytes;//total bytes physically written to this SSD (never transferred; migration writes are added to the destination)
			uint64_t Redirected_writes;//number of write transactions this SSD received due to DPM redirection
		};

		//Per-stream zoning information (each NVMe stream has its own logical address domain in MQSim)
		struct Stream_Zoning
		{
			LPA_type Member_pages;//number of logical pages of the stream's domain on ONE member SSD
			uint64_t Zone_count;//number of zones over the array space (multiple of ssd_no)
			uint64_t Zones_per_ssd;
			std::vector<Zone_Info> Zone_info_table;
			std::vector<std::unordered_set<uint64_t>> Empty_zone_set;//per member SSD: the logical zones that are empty (never written) and currently mapped to that SSD
		};

		//The Migration Info structure of an ongoing data migration process (single migration thread, as in the paper)
		struct Migration_Info
		{
			bool Active;
			stream_id_type Stream_id;
			unsigned int Hot_SSD, Cold_SSD;
			uint64_t Migrating_zone;//logical zone number being migrated (source, currently still on Hot_SSD)
			uint64_t Cold_zone;//logical zone number used as the migration's destination (currently still on Cold_SSD);
			//the two zones' physical locations are swapped only once every page has been copied AND the hot
			//member's old mapping invalidated (see handle_member_phy_transaction_serviced), never at launch --
			//swapping early would let a brand-new write targeting the (still logically empty) Cold_zone alias
			//onto Migrating_zone's physical territory while it is still being read out, racing the migration's
			//own invalidation of that territory and corrupting the hot member's mapping table.
			unsigned int Pending_writes;
			std::unordered_map<LPA_type, LPA_type> Read_to_write_lpa;//member LPA on the hot SSD -> member LPA on the cold SSD
		};

		SSD_Array_Policy_Type policy;//RAID0 (baseline, static striping only) or SWANS (interdisk wear leveling)
		unsigned int ssd_no_in_array;
		unsigned int zone_size_in_pages;
		unsigned int sectors_per_page;
		unsigned int page_size_in_bytes;
		page_status_type full_page_bitmap;
		double th_precautionary_bytes, th_critical_bytes;
		sim_time_type epoch_default, epoch_placement, epoch_migration;
		unsigned int channel_no_per_ssd, chip_no_per_channel, die_no_per_chip, plane_no_per_die, block_no_per_plane;//geometry of one member SSD (used for wear reporting)

		std::vector<Member_SSD> members;
		std::vector<Stream_Zoning> streams;
		std::vector<SSD_Info> ssd_info_table;

		//Parent-child request bookkeeping for RAID-0 request distribution
		std::unordered_map<User_Request*, User_Request*> child_to_parent;
		std::unordered_map<User_Request*, unsigned int> pending_children;

		//WAM state
		sim_time_type t_next_test;
		bool placement_active;

		//DMH state
		Migration_Info migration;
		//Host requests that arrived while a migration was in flight and touched the zone being migrated
		//(source or destination); held back and re-submitted once that migration fully completes, so
		//that no read/write is ever serviced against a zone whose physical location is mid-move.
		std::list<User_Request*> deferred_requests;

		//Statistics
		uint64_t stat_data_placement_processes;//number of zone swaps performed by DPM
		uint64_t stat_redirected_write_transactions;
		uint64_t stat_data_migration_processes;
		uint64_t stat_migrated_pages;
		uint64_t stat_mu_tests;
		uint64_t stat_parent_requests, stat_child_requests;

		void initialize_zoning();
		double compute_mu_bytes() const;//standard deviation of Balance_bytes across member SSDs
		unsigned int hottest_ssd() const;
		unsigned int coldest_ssd() const;
		//Translates an array-level LPA to its member SSD and member-level LPA (read path: no statistics update)
		void map_read(stream_id_type stream_id, LPA_type lpa, unsigned int& member, LPA_type& member_lpa);
		//WAM + DPM for a single page-write transaction, then translation to (member SSD, member LPA)
		void map_write(stream_id_type stream_id, LPA_type lpa, unsigned int transferred_bytes, unsigned int& member, LPA_type& member_lpa);
		void swap_zones(stream_id_type stream_id, uint64_t zone_a, uint64_t zone_b);//swaps the physical locations of two logical zones and updates the empty-zone sets
		void test_write_distribution();//the periodic mu test of the WAM module (Algorithm 1)
		void start_data_migration();//the DMH module (Algorithm 3)
		void handle_child_request_serviced(User_Request* child);
		void handle_member_phy_transaction_serviced(NVM_Transaction_Flash* transaction);//consumes SWANS migration transactions
	};
}

#endif // !SWANS_CONTROLLER_H
