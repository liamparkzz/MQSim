#include "Device_Lifecycle_Monitor.h"
#include "../sim/Sim_Defs.h"

namespace SSD_Components
{
	bool Device_Lifecycle_Monitor::eol_reached = false;
	std::string Device_Lifecycle_Monitor::first_eol_device_id = "";

	void Device_Lifecycle_Monitor::Reset()
	{
		eol_reached = false;
		first_eol_device_id = "";
	}

	void Device_Lifecycle_Monitor::Report_end_of_life(const std::string& device_id, double op_ratio_at_eol, unsigned int bad_block_count)
	{
		if (!eol_reached) {
			eol_reached = true;
			first_eol_device_id = device_id;
		}
		PRINT_MESSAGE("*** " << device_id << " has reached END OF LIFE: remaining over-provisioning ratio "
			<< op_ratio_at_eol << " has dropped to/below the configured threshold (" << bad_block_count << " bad blocks so far) ***")
	}

	bool Device_Lifecycle_Monitor::Has_reached_end_of_life()
	{
		return eol_reached;
	}

	std::string Device_Lifecycle_Monitor::Get_first_eol_device_id()
	{
		return first_eol_device_id;
	}
}
