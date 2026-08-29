#ifndef SSD_DEVICE_H
#define SSD_DEVICE_H

#include <vector>
#include "../sim/Sim_Object.h"
#include "../sim/Sim_Reporter.h"
#include "../ssd/SSD_Defs.h"
#include "../ssd/Host_Interface_Base.h"
#include "../ssd/Host_Interface_SATA.h"
#include "../ssd/Host_Interface_NVMe.h"
#include "../ssd/Data_Cache_Manager_Base.h"
#include "../ssd/Data_Cache_Flash.h"
#include "../ssd/NVM_Firmware.h"
#include "../ssd/NVM_PHY_Base.h"
#include "../ssd/NVM_Channel_Base.h"
#include "../host/PCIe_Switch.h"
#include "../nvm_chip/NVM_Types.h"
#include "Device_Parameter_Set.h"
#include "IO_Flow_Parameter_Set.h"
#include "SWANS_Controller.h"
#include "../utils/Workload_Statistics.h"

/*********************************************************************************************************
* An SSD device has the following components:
*
* Host_Interface <---> Data_Cache_Manager <----> NVM_Firmware <---> NVM_PHY <---> NVM_Channel <---> Chips
*
* When the SWANS RAID-0 array mode is enabled (SWANS_Parameter_Set::Enabled), the device models an
* SSD array instead of a single SSD: N complete and independent flash back-ends (member SSDs) are
* instantiated, and the SWANS array controller is placed between the host interface and the member
* SSDs' cache managers:
*
* Host_Interface <---> SWANS_Controller(RAID-0) <--+--> [SSD0] Data_Cache_Manager <-> FTL <-> PHY <-> Chips
*                                                  +--> [SSD1] Data_Cache_Manager <-> FTL <-> PHY <-> Chips
*                                                  +--> ...
*********************************************************************************************************/

class SSD_Device : public MQSimEngine::Sim_Object, public MQSimEngine::Sim_Reporter
{
public:
	SSD_Device(Device_Parameter_Set* parameters, std::vector<IO_Flow_Parameter_Set*>* io_flows);
	~SSD_Device();
	bool Preconditioning_required;
	NVM::NVM_Type Memory_Type;

	//One complete flash back-end (cache manager + FTL + PHY + channels/chips). In the normal
	//single-SSD mode there is exactly one back-end; in the SWANS RAID-0 mode there is one
	//back-end per member SSD of the array.
	struct Flash_Backend
	{
		std::vector<SSD_Components::NVM_Channel_Base*> Channels;
		SSD_Components::NVM_PHY_Base* PHY;
		SSD_Components::NVM_Firmware* Firmware;
		SSD_Components::Data_Cache_Manager_Base* Cache_manager;
	};

	SSD_Components::Host_Interface_Base *Host_interface;
	SSD_Components::Data_Cache_Manager_Base *Cache_manager;//The cache manager connected to the host interface (the SWANS array controller in the RAID mode)
	SSD_Components::NVM_Firmware* Firmware;//The firmware of the first back-end (used for host-address conversions; all member SSDs are identical)
	SSD_Components::NVM_PHY_Base* PHY;//The PHY of the first back-end
	std::vector<SSD_Components::NVM_Channel_Base*> Channels;//The channels of the first back-end
	std::vector<Flash_Backend> Backends;//All flash back-ends (size is 1, or SSD_No_In_Array in the SWANS RAID-0 mode)
	SSD_Components::SWANS_Controller* SWANS;//The SWANS RAID-0 array controller (NULL if the SWANS mode is disabled)

	void Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter);
	unsigned int Get_no_of_LHAs_in_an_NVM_write_unit();

	void Attach_to_host(Host_Components::PCIe_Switch* pcie_switch);
	void Perform_preconditioning(std::vector<Utils::Workload_Statistics*> workload_stats);
	void Start_simulation();
	void Validate_simulation_config();
	void Execute_simulator_event(MQSimEngine::Sim_Event* event);
	static LPA_type Convert_host_logical_address_to_device_address(LHA_type lha);
	static page_status_type Find_NVM_subunit_access_bitmap(LHA_type lha);

	unsigned int Channel_count;//Number of channels of ONE back-end (member SSD)
	unsigned int Chip_no_per_channel;

private:
	static SSD_Device * my_instance;//Used in static functions
	Flash_Backend build_flash_backend(Device_Parameter_Set* parameters, std::vector<IO_Flow_Parameter_Set*>* io_flows,
		const sim_object_id_type& id_prefix, unsigned int stream_count,
		sim_time_type* read_latencies, sim_time_type* write_latencies,
		sim_time_type average_flash_read_latency, sim_time_type average_flash_write_latency,
		const std::vector<std::vector<flash_channel_ID_type>>& flow_channel_id_assignments,
		const std::vector<std::vector<flash_chip_ID_type>>& flow_chip_id_assignments,
		const std::vector<std::vector<flash_die_ID_type>>& flow_die_id_assignments,
		const std::vector<std::vector<flash_plane_ID_type>>& flow_plane_id_assignments);
};

#endif //!SSD_DEVICE_H
