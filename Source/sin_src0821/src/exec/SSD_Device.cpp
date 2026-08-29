#include <vector>
#include <stdexcept>
#include <ctime>
#include "SSD_Device.h"
#include "../ssd/ONFI_Channel_Base.h"
#include "../ssd/Flash_Block_Manager.h"
#include "../ssd/Data_Cache_Manager_Flash_Advanced.h"
#include "../ssd/Data_Cache_Manager_Flash_Simple.h"
#include "../ssd/Address_Mapping_Unit_Base.h"
#include "../ssd/Address_Mapping_Unit_Page_Level.h"
#include "../ssd/Address_Mapping_Unit_Hybrid.h"
#include "../ssd/GC_and_WL_Unit_Page_Level.h"
#include "../ssd/TSU_OutofOrder.h"
#include "../ssd/TSU_Priority_OutOfOrder.h"
#include "../ssd/TSU_FLIN.h"
#include "../ssd/ONFI_Channel_NVDDR2.h"
#include "../ssd/NVM_PHY_ONFI_NVDDR2.h"
#include "../utils/Logical_Address_Partitioning_Unit.h"

SSD_Device *SSD_Device::my_instance; //Used in static functions

//Builds one complete flash back-end: channels/chips, PHY, FTL (TSU, block manager, AMU, GC/WL), and
//the data cache manager. In the normal mode one back-end is built; in the SWANS RAID-0 mode this
//function is called once per member SSD of the array (each member SSD is a complete and independent
//SSD instance with its own cache, FTL, GC, and wear state).
SSD_Device::Flash_Backend SSD_Device::build_flash_backend(Device_Parameter_Set *parameters, std::vector<IO_Flow_Parameter_Set *> *io_flows,
	const sim_object_id_type& id_prefix, unsigned int stream_count,
	sim_time_type* read_latencies, sim_time_type* write_latencies,
	sim_time_type average_flash_read_latency, sim_time_type average_flash_write_latency,
	const std::vector<std::vector<flash_channel_ID_type>>& flow_channel_id_assignments,
	const std::vector<std::vector<flash_chip_ID_type>>& flow_chip_id_assignments,
	const std::vector<std::vector<flash_die_ID_type>>& flow_die_id_assignments,
	const std::vector<std::vector<flash_plane_ID_type>>& flow_plane_id_assignments)
{
	Flash_Backend backend;

	//Step 2: create memory channels to connect chips to the controller
	switch (parameters->Flash_Comm_Protocol)
	{
	case SSD_Components::ONFI_Protocol::NVDDR2:
	{
		SSD_Components::ONFI_Channel_NVDDR2 **channels = new SSD_Components::ONFI_Channel_NVDDR2 *[parameters->Flash_Channel_Count];
		for (unsigned int channel_cntr = 0; channel_cntr < parameters->Flash_Channel_Count; channel_cntr++)
		{
			NVM::FlashMemory::Flash_Chip **chips = new NVM::FlashMemory::Flash_Chip *[parameters->Chip_No_Per_Channel];
			for (unsigned int chip_cntr = 0; chip_cntr < parameters->Chip_No_Per_Channel; chip_cntr++)
			{
				chips[chip_cntr] = new NVM::FlashMemory::Flash_Chip(id_prefix + ".Channel." + std::to_string(channel_cntr) + ".Chip." + std::to_string(chip_cntr),
																	channel_cntr, chip_cntr, parameters->Flash_Parameters.Flash_Technology, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
																	parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
																	read_latencies, write_latencies, parameters->Flash_Parameters.Block_Erase_Latency,
																	parameters->Flash_Parameters.Suspend_Program_Time, parameters->Flash_Parameters.Suspend_Erase_Time);
				Simulator->AddObject(chips[chip_cntr]); //Each simulation object (a child of MQSimEngine::Sim_Object) should be added to the engine
			}
			channels[channel_cntr] = new SSD_Components::ONFI_Channel_NVDDR2(channel_cntr, parameters->Chip_No_Per_Channel,
																			 chips, parameters->Flash_Channel_Width,
																			 (sim_time_type)((double)1000 / parameters->Channel_Transfer_Rate) * 2, (sim_time_type)((double)1000 / parameters->Channel_Transfer_Rate) * 2);
			backend.Channels.push_back(channels[channel_cntr]); //Channels should not be added to the simulator core, they are passive object that do not handle any simulation event
		}

		//Step 3: create channel controller and connect channels to it
		backend.PHY = new SSD_Components::NVM_PHY_ONFI_NVDDR2(id_prefix + ".PHY", channels, parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
															  parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die);
		Simulator->AddObject(backend.PHY);
		break;
	}
	default:
		throw std::invalid_argument("No implementation is available for the specified flash communication protocol");
	}

	//Steps 4 - 8: create FTL components and connect them together
	SSD_Components::FTL *ftl = new SSD_Components::FTL(id_prefix + ".FTL", NULL, parameters->Flash_Channel_Count,
													   parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
													   parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
													   parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, average_flash_read_latency, average_flash_write_latency, parameters->Overprovisioning_Ratio,
													   parameters->Flash_Parameters.Block_PE_Cycles_Limit, parameters->Seed++);
	ftl->PHY = (SSD_Components::NVM_PHY_ONFI *)backend.PHY;
	Simulator->AddObject(ftl);
	backend.Firmware = ftl;

	//Step 5: create TSU
	SSD_Components::TSU_Base *tsu;
	bool erase_suspension = false, program_suspension = false;
	if (parameters->Flash_Parameters.CMD_Suspension_Support == NVM::FlashMemory::Command_Suspension_Mode::PROGRAM)
	{
		program_suspension = true;
	}
	if (parameters->Flash_Parameters.CMD_Suspension_Support == NVM::FlashMemory::Command_Suspension_Mode::ERASE)
	{
		erase_suspension = true;
	}
	if (parameters->Flash_Parameters.CMD_Suspension_Support == NVM::FlashMemory::Command_Suspension_Mode::PROGRAM_ERASE)
	{
		program_suspension = true;
		erase_suspension = true;
	}
	switch (parameters->Transaction_Scheduling_Policy)
	{
	case SSD_Components::Flash_Scheduling_Type::OUT_OF_ORDER:
		tsu = new SSD_Components::TSU_OutOfOrder(ftl->ID() + ".TSU", ftl, static_cast<SSD_Components::NVM_PHY_ONFI_NVDDR2 *>(backend.PHY),
												 parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
												 parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
												 parameters->Preferred_suspend_write_time_for_read, parameters->Preferred_suspend_erase_time_for_read,
												 parameters->Preferred_suspend_erase_time_for_write,
												 erase_suspension, program_suspension);
		break;
	case SSD_Components::Flash_Scheduling_Type::PRIORITY_OUT_OF_ORDER:
		tsu = new SSD_Components::TSU_Priority_OutOfOrder(ftl->ID() + ".TSU", ftl, static_cast<SSD_Components::NVM_PHY_ONFI_NVDDR2 *>(backend.PHY),
									  parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
									  parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
									  parameters->Preferred_suspend_write_time_for_read, parameters->Preferred_suspend_erase_time_for_read,
									  parameters->Preferred_suspend_erase_time_for_write,
									  erase_suspension, program_suspension);
		break;
	default:
		throw std::invalid_argument("No implementation is available for the specified transaction scheduling algorithm");
	}
	Simulator->AddObject(tsu);
	ftl->TSU = tsu;

	//Step 6: create Flash_Block_Manager
	SSD_Components::Flash_Block_Manager_Base *fbm;
	fbm = new SSD_Components::Flash_Block_Manager(id_prefix + ".BlockManager", NULL, parameters->Flash_Parameters.Block_PE_Cycles_Limit,
												  (unsigned int)io_flows->size(), parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
												  parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
												  parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
												  parameters->Overprovisioning_Ratio, parameters->End_of_Life_Threshold);
	ftl->BlockManager = fbm;

	//Step 7: create Address_Mapping_Unit
	SSD_Components::Address_Mapping_Unit_Base *amu;
	switch (parameters->Address_Mapping)
	{
	case SSD_Components::Flash_Address_Mapping_Type::PAGE_LEVEL:
		amu = new SSD_Components::Address_Mapping_Unit_Page_Level(ftl->ID() + ".AddressMappingUnit", ftl, (SSD_Components::NVM_PHY_ONFI *)backend.PHY,
																  fbm, parameters->Ideal_Mapping_Table, parameters->CMT_Capacity, parameters->Plane_Allocation_Scheme, stream_count,
																  parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
																  flow_channel_id_assignments, flow_chip_id_assignments, flow_die_id_assignments, flow_plane_id_assignments,
																  parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
																  parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Flash_Parameters.Page_Capacity, parameters->Overprovisioning_Ratio,
																  parameters->CMT_Sharing_Mode);
		break;
	case SSD_Components::Flash_Address_Mapping_Type::HYBRID:
		amu = new SSD_Components::Address_Mapping_Unit_Hybrid(ftl->ID() + ".AddressMappingUnit", ftl, (SSD_Components::NVM_PHY_ONFI *)backend.PHY,
															  fbm, parameters->Ideal_Mapping_Table, stream_count,
															  parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip,
															  parameters->Flash_Parameters.Plane_No_Per_Die, parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
															  parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Flash_Parameters.Page_Capacity, parameters->Overprovisioning_Ratio);
		break;
	default:
		throw std::invalid_argument("No implementation is available fo the secified address mapping strategy");
	}
	Simulator->AddObject(amu);
	ftl->Address_Mapping_Unit = amu;

	//Step 8: create GC_and_WL_unit
	double max_rho = 0;
	for (unsigned int i = 0; i < io_flows->size(); i++)
	{
		if ((*io_flows)[i]->Initial_Occupancy_Percentage > max_rho)
		{
			max_rho = (*io_flows)[i]->Initial_Occupancy_Percentage;
		}
	}
	max_rho /= 100; //Convert from percentage to a value between zero and 1
	SSD_Components::GC_and_WL_Unit_Base *gcwl;
	gcwl = new SSD_Components::GC_and_WL_Unit_Page_Level(ftl->ID() + ".GCandWLUnit", amu, fbm, tsu, (SSD_Components::NVM_PHY_ONFI *)backend.PHY,
														 parameters->GC_Block_Selection_Policy, parameters->GC_Exec_Threshold, parameters->Preemptible_GC_Enabled, parameters->GC_Hard_Threshold,
														 parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
														 parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
														 parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
														 parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Use_Copyback_for_GC, max_rho, 10,
														 parameters->Seed++);
	Simulator->AddObject(gcwl);
	fbm->Set_GC_and_WL_Unit(gcwl);
	ftl->GC_and_WL_Unit = gcwl;

	//Step 9: create Data_Cache_Manager
	SSD_Components::Data_Cache_Manager_Base *dcm;
	SSD_Components::Caching_Mode *caching_modes = new SSD_Components::Caching_Mode[io_flows->size()];
	for (unsigned int i = 0; i < io_flows->size(); i++)
	{
		caching_modes[i] = (*io_flows)[i]->Device_Level_Data_Caching_Mode;
	}

	switch (parameters->Caching_Mechanism)
	{
	case SSD_Components::Caching_Mechanism::SIMPLE:
		dcm = new SSD_Components::Data_Cache_Manager_Flash_Simple(id_prefix + ".DataCache", NULL, ftl, (SSD_Components::NVM_PHY_ONFI *)backend.PHY,
																  parameters->Data_Cache_Capacity, parameters->Data_Cache_DRAM_Row_Size, parameters->Data_Cache_DRAM_Data_Rate,
																  parameters->Data_Cache_DRAM_Data_Busrt_Size, parameters->Data_Cache_DRAM_tRCD, parameters->Data_Cache_DRAM_tCL, parameters->Data_Cache_DRAM_tRP,
																  caching_modes, (unsigned int)io_flows->size(),
																  parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Flash_Channel_Count * parameters->Chip_No_Per_Channel * parameters->Flash_Parameters.Die_No_Per_Chip * parameters->Flash_Parameters.Plane_No_Per_Die * parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE);

		break;
	case SSD_Components::Caching_Mechanism::ADVANCED:
		dcm = new SSD_Components::Data_Cache_Manager_Flash_Advanced(id_prefix + ".DataCache", NULL, ftl, (SSD_Components::NVM_PHY_ONFI *)backend.PHY,
																	parameters->Data_Cache_Capacity, parameters->Data_Cache_DRAM_Row_Size, parameters->Data_Cache_DRAM_Data_Rate,
																	parameters->Data_Cache_DRAM_Data_Busrt_Size, parameters->Data_Cache_DRAM_tRCD, parameters->Data_Cache_DRAM_tCL, parameters->Data_Cache_DRAM_tRP,
																	caching_modes, parameters->Data_Cache_Sharing_Mode, (unsigned int)io_flows->size(),
																	parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Flash_Channel_Count * parameters->Chip_No_Per_Channel * parameters->Flash_Parameters.Die_No_Per_Chip * parameters->Flash_Parameters.Plane_No_Per_Die * parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE);

		break;
	default:
		PRINT_ERROR("Unknown data caching mechanism!")
	}

	Simulator->AddObject(dcm);
	ftl->Data_cache_manager = dcm;
	backend.Cache_manager = dcm;

	return backend;
}

SSD_Device::SSD_Device(Device_Parameter_Set *parameters, std::vector<IO_Flow_Parameter_Set *> *io_flows) : MQSimEngine::Sim_Object("SSDDevice")
{
	SSD_Device *device = this;
	my_instance = device; //used for static functions
	Simulator->AddObject(device);

	device->Preconditioning_required = parameters->Enabled_Preconditioning;
	device->Memory_Type = parameters->Memory_Type;
	device->SWANS = NULL;

	switch (Memory_Type)
	{
	case NVM::NVM_Type::FLASH:
	{
		sim_time_type *read_latencies, *write_latencies;
		sim_time_type average_flash_read_latency = 0, average_flash_write_latency = 0; //Required for FTL initialization

		//Step 1: determine the flash latencies used for creating memory chips
		switch (parameters->Flash_Parameters.Flash_Technology)
		{
		case Flash_Technology_Type::SLC:
			read_latencies = new sim_time_type[1];
			read_latencies[0] = parameters->Flash_Parameters.Page_Read_Latency_LSB;
			write_latencies = new sim_time_type[1];
			write_latencies[0] = parameters->Flash_Parameters.Page_Program_Latency_LSB;
			average_flash_read_latency = read_latencies[0];
			average_flash_write_latency = write_latencies[0];
			break;
		case Flash_Technology_Type::MLC:
			read_latencies = new sim_time_type[2];
			read_latencies[0] = parameters->Flash_Parameters.Page_Read_Latency_LSB;
			read_latencies[1] = parameters->Flash_Parameters.Page_Read_Latency_MSB;
			write_latencies = new sim_time_type[2];
			write_latencies[0] = parameters->Flash_Parameters.Page_Program_Latency_LSB;
			write_latencies[1] = parameters->Flash_Parameters.Page_Program_Latency_MSB;
			average_flash_read_latency = (read_latencies[0] + read_latencies[1]) / 2;
			average_flash_write_latency = (write_latencies[0] + write_latencies[1]) / 2;
			break;
		case Flash_Technology_Type::TLC:
			read_latencies = new sim_time_type[3];
			read_latencies[0] = parameters->Flash_Parameters.Page_Read_Latency_LSB;
			read_latencies[1] = parameters->Flash_Parameters.Page_Read_Latency_CSB;
			read_latencies[2] = parameters->Flash_Parameters.Page_Read_Latency_MSB;
			write_latencies = new sim_time_type[3];
			write_latencies[0] = parameters->Flash_Parameters.Page_Program_Latency_LSB;
			write_latencies[1] = parameters->Flash_Parameters.Page_Program_Latency_CSB;
			write_latencies[2] = parameters->Flash_Parameters.Page_Program_Latency_MSB;
			average_flash_read_latency = (read_latencies[0] + read_latencies[1] + read_latencies[2]) / 3;
			average_flash_write_latency = (write_latencies[0] + write_latencies[1] + write_latencies[2]) / 3;
			break;
		default:
			throw std::invalid_argument("The specified flash technologies is not supported");
		}

		this->Channel_count = parameters->Flash_Channel_Count;
		this->Chip_no_per_channel = parameters->Chip_No_Per_Channel;

		//Determine the resource (channel/chip/die/plane) assignment of the concurrent I/O flows
		std::vector<std::vector<flash_channel_ID_type>> flow_channel_id_assignments;
		std::vector<std::vector<flash_chip_ID_type>> flow_chip_id_assignments;
		std::vector<std::vector<flash_die_ID_type>> flow_die_id_assignments;
		std::vector<std::vector<flash_plane_ID_type>> flow_plane_id_assignments;
		unsigned int stream_count = 0;
		for (unsigned int i = 0; i < io_flows->size(); i++)
		{
			switch (parameters->HostInterface_Type)
			{
			case HostInterface_Types::SATA:
			{
				stream_count = 1;
				std::vector<flash_channel_ID_type> channel_ids;
				flow_channel_id_assignments.push_back(channel_ids);
				for (unsigned int j = 0; j < parameters->Flash_Channel_Count; j++)
				{
					flow_channel_id_assignments[i].push_back(j);
				}
				std::vector<flash_chip_ID_type> chip_ids;
				flow_chip_id_assignments.push_back(chip_ids);
				for (unsigned int j = 0; j < parameters->Chip_No_Per_Channel; j++)
				{
					flow_chip_id_assignments[i].push_back(j);
				}
				std::vector<flash_die_ID_type> die_ids;
				flow_die_id_assignments.push_back(die_ids);
				for (unsigned int j = 0; j < parameters->Flash_Parameters.Die_No_Per_Chip; j++)
				{
					flow_die_id_assignments[i].push_back(j);
				}
				std::vector<flash_plane_ID_type> plane_ids;
				flow_plane_id_assignments.push_back(plane_ids);
				for (unsigned int j = 0; j < parameters->Flash_Parameters.Plane_No_Per_Die; j++)
				{
					flow_plane_id_assignments[i].push_back(j);
				}
				break;
			}
			case HostInterface_Types::NVME:
			{
				stream_count = (unsigned int)io_flows->size();
				std::vector<flash_channel_ID_type> channel_ids;
				flow_channel_id_assignments.push_back(channel_ids);
				for (int j = 0; j < (*io_flows)[i]->Channel_No; j++)
				{
					flow_channel_id_assignments[i].push_back((*io_flows)[i]->Channel_IDs[j]);
				}
				std::vector<flash_chip_ID_type> chip_ids;
				flow_chip_id_assignments.push_back(chip_ids);
				for (int j = 0; j < (*io_flows)[i]->Chip_No; j++)
				{
					flow_chip_id_assignments[i].push_back((*io_flows)[i]->Chip_IDs[j]);
				}
				std::vector<flash_die_ID_type> die_ids;
				flow_die_id_assignments.push_back(die_ids);
				for (int j = 0; j < (*io_flows)[i]->Die_No; j++)
				{
					flow_die_id_assignments[i].push_back((*io_flows)[i]->Die_IDs[j]);
				}
				std::vector<flash_plane_ID_type> plane_ids;
				flow_plane_id_assignments.push_back(plane_ids);
				for (int j = 0; j < (*io_flows)[i]->Plane_No; j++)
				{
					flow_plane_id_assignments[i].push_back((*io_flows)[i]->Plane_IDs[j]);
				}
				break;
			}
			default:
				break;
			}
		}

		//The logical address space partitioning is performed for the geometry of ONE back-end (i.e., one
		//member SSD in the SWANS RAID-0 mode); the host-visible space of the array is scaled afterwards.
		Utils::Logical_Address_Partitioning_Unit::Allocate_logical_address_for_flows(parameters->HostInterface_Type, (unsigned int)io_flows->size(),
																					 parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
																					 flow_channel_id_assignments, flow_chip_id_assignments, flow_die_id_assignments, flow_plane_id_assignments,
																					 parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
																					 parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Overprovisioning_Ratio);

		//Determine the number of flash back-ends to build: 1 in the normal single-SSD mode, or
		//SSD_No_In_Array in the SWANS RAID-0 mode (each member SSD is a full and independent back-end)
		unsigned int backend_no = 1;
		if (SWANS_Parameter_Set::Enabled)
		{
			if (parameters->Address_Mapping != SSD_Components::Flash_Address_Mapping_Type::PAGE_LEVEL) {
				PRINT_ERROR("SWANS requires the PAGE_LEVEL address mapping!")
			}
			if (parameters->Transaction_Scheduling_Policy == SSD_Components::Flash_Scheduling_Type::FLIN) {
				PRINT_ERROR("SWANS cannot be used with the FLIN transaction scheduling policy!")
			}
			if (SWANS_Parameter_Set::SSD_No_In_Array < 2) {
				PRINT_ERROR("SWANS: SSD_No_In_Array must be at least 2!")
			}
			if (device->Preconditioning_required) {
				PRINT_MESSAGE("SWANS: preconditioning is not supported in the SWANS RAID mode and is disabled.")
				device->Preconditioning_required = false;
			}
			backend_no = SWANS_Parameter_Set::SSD_No_In_Array;
		}

		for (unsigned int ssd_cntr = 0; ssd_cntr < backend_no; ssd_cntr++)
		{
			sim_object_id_type backend_id = (backend_no == 1 ? device->ID() : device->ID() + ".SSD" + std::to_string(ssd_cntr));
			Backends.push_back(build_flash_backend(parameters, io_flows, backend_id, stream_count,
												   read_latencies, write_latencies, average_flash_read_latency, average_flash_write_latency,
												   flow_channel_id_assignments, flow_chip_id_assignments, flow_die_id_assignments, flow_plane_id_assignments));
		}
		delete[] read_latencies;
		delete[] write_latencies;

		device->Channels = Backends[0].Channels;
		device->PHY = Backends[0].PHY;
		device->Firmware = Backends[0].Firmware;

		//Step 9.5: create the SWANS RAID-0 array controller (if enabled) and register the member SSDs
		if (SWANS_Parameter_Set::Enabled)
		{
			unsigned int zone_size_in_pages = (unsigned int)(((uint64_t)SWANS_Parameter_Set::Zone_Size_MB * 1024 * 1024) / parameters->Flash_Parameters.Page_Capacity);
			SSD_Components::Caching_Mode *caching_modes = new SSD_Components::Caching_Mode[io_flows->size()];
			for (unsigned int i = 0; i < io_flows->size(); i++) {
				caching_modes[i] = (*io_flows)[i]->Device_Level_Data_Caching_Mode;
			}
			device->SWANS = new SSD_Components::SWANS_Controller(device->ID() + ".ArrayController", SWANS_Parameter_Set::Policy,
				SWANS_Parameter_Set::SSD_No_In_Array,
				zone_size_in_pages, parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE,
				SWANS_Parameter_Set::Threshold_Precautionary_MB * 1024 * 1024, SWANS_Parameter_Set::Threshold_Critical_MB * 1024 * 1024,
				SWANS_Parameter_Set::Epoch_Default_ms * 1000000, SWANS_Parameter_Set::Epoch_Placement_ms * 1000000, SWANS_Parameter_Set::Epoch_Migration_ms * 1000000,
				caching_modes, stream_count,
				parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip,
				parameters->Flash_Parameters.Plane_No_Per_Die, parameters->Flash_Parameters.Block_No_Per_Plane);
			for (unsigned int ssd_cntr = 0; ssd_cntr < backend_no; ssd_cntr++) {
				device->SWANS->Add_member_ssd((SSD_Components::FTL *)Backends[ssd_cntr].Firmware, Backends[ssd_cntr].Cache_manager,
					(SSD_Components::NVM_PHY_ONFI *)Backends[ssd_cntr].PHY);
			}
			Simulator->AddObject(device->SWANS);
			device->Cache_manager = device->SWANS;

			//The host-visible logical space of the array is (SSD_No_In_Array x one member SSD)
			Utils::Logical_Address_Partitioning_Unit::Set_address_space_multiplier(SWANS_Parameter_Set::SSD_No_In_Array);

			PRINT_MESSAGE("SSD array (RAID-0) mode is enabled with the "
				<< (SWANS_Parameter_Set::Policy == SSD_Array_Policy_Type::SWANS ? "SWANS" : "plain RAID0 (baseline)") << " policy: "
				<< SWANS_Parameter_Set::SSD_No_In_Array
				<< " member SSDs (each with " << parameters->Flash_Channel_Count << " channels), zone size = "
				<< SWANS_Parameter_Set::Zone_Size_MB << "MB")
		}
		else
		{
			device->Cache_manager = Backends[0].Cache_manager;
		}

		//Step 10: create Host_Interface
		switch (parameters->HostInterface_Type)
		{
		case HostInterface_Types::NVME:
			device->Host_interface = new SSD_Components::Host_Interface_NVMe(device->ID() + ".HostInterface",
																			 Utils::Logical_Address_Partitioning_Unit::Get_total_device_lha_count(), parameters->IO_Queue_Depth, parameters->IO_Queue_Depth,
																			 (unsigned int)io_flows->size(), parameters->Queue_Fetch_Size, parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, device->Cache_manager);
			break;
		case HostInterface_Types::SATA:
			device->Host_interface = new SSD_Components::Host_Interface_SATA(device->ID() + ".HostInterface",
																			 parameters->IO_Queue_Depth, Utils::Logical_Address_Partitioning_Unit::Get_total_device_lha_count(), parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, device->Cache_manager);

			break;
		default:
			break;
		}
		Simulator->AddObject(device->Host_interface);
		device->Cache_manager->Set_host_interface(device->Host_interface);
		break;
	}
	default:
		throw std::invalid_argument("Undefined NVM type specified ");
	} // switch (Memory_Type)
}

SSD_Device::~SSD_Device()
{
	for (unsigned int backend_cntr = 0; backend_cntr < Backends.size(); backend_cntr++)
	{
		for (unsigned int channel_cntr = 0; channel_cntr < Channel_count; channel_cntr++)
		{
			for (unsigned int chip_cntr = 0; chip_cntr < Chip_no_per_channel; chip_cntr++)
			{
				delete ((SSD_Components::ONFI_Channel_NVDDR2 *)Backends[backend_cntr].Channels[channel_cntr])->Chips[chip_cntr];
			}
			delete Backends[backend_cntr].Channels[channel_cntr];
		}

		delete Backends[backend_cntr].PHY;
		delete ((SSD_Components::FTL *)Backends[backend_cntr].Firmware)->TSU;
		delete ((SSD_Components::FTL *)Backends[backend_cntr].Firmware)->BlockManager;
		delete ((SSD_Components::FTL *)Backends[backend_cntr].Firmware)->Address_Mapping_Unit;
		delete ((SSD_Components::FTL *)Backends[backend_cntr].Firmware)->GC_and_WL_Unit;
		delete Backends[backend_cntr].Firmware;
		delete Backends[backend_cntr].Cache_manager;
	}

	if (this->SWANS != NULL)
	{
		delete this->SWANS;
	}
	delete this->Host_interface;
}

void SSD_Device::Attach_to_host(Host_Components::PCIe_Switch *pcie_switch)
{
	this->Host_interface->Attach_to_device(pcie_switch);
}

void SSD_Device::Perform_preconditioning(std::vector<Utils::Workload_Statistics *> workload_stats)
{
	if (Preconditioning_required)
	{
		time_t start_time = time(0);
		PRINT_MESSAGE("SSD Device preconditioning started .........");
		this->Firmware->Perform_precondition(workload_stats);
		this->Cache_manager->Do_warmup(workload_stats);
		time_t end_time = time(0);
		uint64_t duration = (uint64_t)difftime(end_time, start_time);
		PRINT_MESSAGE("Finished preconditioning. Duration of preconditioning: " << duration / 3600 << ":" << (duration % 3600) / 60 << ":" << ((duration % 3600) % 60));
	}
}

void SSD_Device::Start_simulation()
{
}

void SSD_Device::Validate_simulation_config()
{
}

void SSD_Device::Execute_simulator_event(MQSimEngine::Sim_Event *event)
{
}

void SSD_Device::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter &xmlwriter)
{
	std::string tmp;
	tmp = ID();
	xmlwriter.Write_open_tag(tmp);

	this->Host_interface->Report_results_in_XML(ID(), xmlwriter);
	if (this->SWANS != NULL)
	{
		this->SWANS->Report_results_in_XML(ID(), xmlwriter);
	}
	if (Memory_Type == NVM::NVM_Type::FLASH)
	{
		//Note: most of the FTL counters are process-wide (SSD_Components::Stats), so in the SWANS
		//RAID-0 mode the FTL report contains the aggregate statistics of all member SSDs.
		((SSD_Components::FTL *)this->Firmware)->Report_results_in_XML(ID(), xmlwriter);

		for (unsigned int backend_cntr = 0; backend_cntr < Backends.size(); backend_cntr++)
		{
			((SSD_Components::FTL *)Backends[backend_cntr].Firmware)->TSU->Report_results_in_XML(ID(), xmlwriter);

			for (unsigned int channel_cntr = 0; channel_cntr < Channel_count; channel_cntr++)
			{
				for (unsigned int chip_cntr = 0; chip_cntr < Chip_no_per_channel; chip_cntr++)
				{
					((SSD_Components::ONFI_Channel_NVDDR2 *)Backends[backend_cntr].Channels[channel_cntr])->Chips[chip_cntr]->Report_results_in_XML(ID(), xmlwriter);
				}
			}

			//Bad-block / end-of-life summary of this back-end (the whole device in single-SSD mode,
			//or one member SSD of a RAID/SWANS array)
			SSD_Components::Flash_Block_Manager_Base* bm = ((SSD_Components::FTL *)Backends[backend_cntr].Firmware)->BlockManager;
			std::string lifecycle_tag = (Backends.size() == 1) ? std::string("Lifecycle") : ("SSD" + std::to_string(backend_cntr) + ".Lifecycle");
			xmlwriter.Write_open_tag(lifecycle_tag);
			xmlwriter.Write_attribute_string(std::string("Bad_Block_Count"), std::to_string(bm->Get_bad_block_count()));
			xmlwriter.Write_attribute_string(std::string("Current_OP_Ratio"), std::to_string(bm->Get_current_op_ratio()));
			xmlwriter.Write_attribute_string(std::string("Reached_End_Of_Life"), bm->Has_reached_end_of_life() ? "true" : "false");
			xmlwriter.Write_close_tag();
		}
	}
	xmlwriter.Write_close_tag();
}

unsigned int SSD_Device::Get_no_of_LHAs_in_an_NVM_write_unit()
{
	return Host_interface->Get_no_of_LHAs_in_an_NVM_write_unit();
}

LPA_type SSD_Device::Convert_host_logical_address_to_device_address(LHA_type lha)
{
	return my_instance->Firmware->Convert_host_logical_address_to_device_address(lha);
}

page_status_type SSD_Device::Find_NVM_subunit_access_bitmap(LHA_type lha)
{
	return my_instance->Firmware->Find_NVM_subunit_access_bitmap(lha);
}
