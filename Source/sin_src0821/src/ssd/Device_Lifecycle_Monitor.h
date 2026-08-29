#ifndef DEVICE_LIFECYCLE_MONITOR_H
#define DEVICE_LIFECYCLE_MONITOR_H

#include <string>

namespace SSD_Components
{
	/* A tiny, dependency-light global flag that lets the host side (I/O flow generators) find out
	*  when any flash back-end (a single SSD, or any member SSD of a RAID/SWANS array) has reached
	*  its end of life, without the host-side code having to depend on the heavy SSD_Device/FTL
	*  header chain. A device reaches end of life when its remaining over-provisioning (spare block)
	*  ratio drops to or below the configured End_of_Life_Threshold (see Flash_Block_Manager_Base).
	*
	*  This is used to implement the UNTIL_END_OF_LIFE trace replay mode (see
	*  IO_Flow_Parameter_Set_Trace_Based::Relay_Mode): instead of replaying a trace a fixed number of
	*  times, the flow keeps replaying it until any device in the (possibly multi-SSD) array reports
	*  end of life here.*/
	class Device_Lifecycle_Monitor
	{
	public:
		static void Reset();//Called on Simulator->Reset() so end-of-life state does not leak across scenarios
		static void Report_end_of_life(const std::string& device_id, double op_ratio_at_eol, unsigned int bad_block_count);
		static bool Has_reached_end_of_life();
		static std::string Get_first_eol_device_id();//empty string if no device has reached end of life yet
	private:
		static bool eol_reached;
		static std::string first_eol_device_id;
	};
}

#endif // !DEVICE_LIFECYCLE_MONITOR_H
