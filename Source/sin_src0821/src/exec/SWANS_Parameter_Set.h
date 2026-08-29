#ifndef SWANS_PARAMETER_SET_H
#define SWANS_PARAMETER_SET_H

#include "../sim/Sim_Defs.h"
#include "Parameter_Set_Base.h"

//The data distribution policy of the SSD array controller
enum class SSD_Array_Policy_Type {
	RAID0, //plain RAID-0: static round-robin zone striping without any array-level wear leveling (the baseline configuration of the SWANS paper)
	SWANS  //RAID-0 + the SWANS interdisk wear-leveling strategy (write redirection and zone migration)
};

/*
* Configuration parameters of the SSD-array (RAID-0) simulation mode and of the SWANS
* (Smoothing Wear Across N SSDs) interdisk wear-leveling strategy.
* (W. Wang, T. Xie, and A. Sharma, "SWANS: An Interdisk Wear-Leveling Strategy for
*  RAID-0 Structured SSD Arrays", ACM Transactions on Storage, 2016)
*/
class SWANS_Parameter_Set : public Parameter_Set_Base
{
public:
	static bool Enabled;//Enables the SSD-array (RAID-0) simulation mode
	static SSD_Array_Policy_Type Policy;//Array-level data distribution policy: RAID0 (baseline) or SWANS
	static unsigned int SSD_No_In_Array;//Number of member SSDs in the RAID-0 array
	static unsigned int Zone_Size_MB;//The size of a SWANS zone in megabytes (paper default: 16MB)
	static double Threshold_Precautionary_MB;//th_precautionary: if the standard deviation of written data (MB) across SSDs exceeds this value, data placement (write redirection) is launched
	static double Threshold_Critical_MB;//th_critical: if the standard deviation of written data (MB) across SSDs exceeds this value, data migration is launched
	static sim_time_type Epoch_Default_ms;//t_test-cycle increment after a test with no action, in milliseconds of simulated time (paper default: 40s)
	static sim_time_type Epoch_Placement_ms;//t_test-cycle increment after a data placement process is launched
	static sim_time_type Epoch_Migration_ms;//t_test-cycle increment after a data migration process is launched
	void XML_serialize(Utils::XmlWriter& xmlwriter);
	void XML_deserialize(rapidxml::xml_node<> *node);
};

#endif // !SWANS_PARAMETER_SET_H
