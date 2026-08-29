#include <algorithm>
#include <string.h>
#include "../sim/Engine.h"
#include "SWANS_Parameter_Set.h"

bool SWANS_Parameter_Set::Enabled = false;
SSD_Array_Policy_Type SWANS_Parameter_Set::Policy = SSD_Array_Policy_Type::SWANS;
unsigned int SWANS_Parameter_Set::SSD_No_In_Array = 4;
unsigned int SWANS_Parameter_Set::Zone_Size_MB = 16;
double SWANS_Parameter_Set::Threshold_Precautionary_MB = 5120;//th(5GB, 15GB) as in the SWANS paper
double SWANS_Parameter_Set::Threshold_Critical_MB = 15360;
sim_time_type SWANS_Parameter_Set::Epoch_Default_ms = 40000;//40s as in the SWANS paper
sim_time_type SWANS_Parameter_Set::Epoch_Placement_ms = 80000;
sim_time_type SWANS_Parameter_Set::Epoch_Migration_ms = 120000;

void SWANS_Parameter_Set::XML_serialize(Utils::XmlWriter& xmlwriter)
{
	std::string tmp;
	tmp = "SWANS_Parameter_Set";
	xmlwriter.Write_open_tag(tmp);

	std::string attr = "Enabled";
	std::string val = (Enabled ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Policy";
	switch (Policy) {
		case SSD_Array_Policy_Type::RAID0:
			val = "RAID0";
			break;
		case SSD_Array_Policy_Type::SWANS:
			val = "SWANS";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "SSD_No_In_Array";
	val = std::to_string(SSD_No_In_Array);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Zone_Size_MB";
	val = std::to_string(Zone_Size_MB);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Threshold_Precautionary_MB";
	val = std::to_string(Threshold_Precautionary_MB);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Threshold_Critical_MB";
	val = std::to_string(Threshold_Critical_MB);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Epoch_Default_ms";
	val = std::to_string(Epoch_Default_ms);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Epoch_Placement_ms";
	val = std::to_string(Epoch_Placement_ms);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Epoch_Migration_ms";
	val = std::to_string(Epoch_Migration_ms);
	xmlwriter.Write_attribute_string(attr, val);

	xmlwriter.Write_close_tag();
}

void SWANS_Parameter_Set::XML_deserialize(rapidxml::xml_node<> *node)
{
	try {
		for (auto param = node->first_node(); param; param = param->next_sibling()) {
			if (strcmp(param->name(), "Enabled") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				Enabled = (val.compare("FALSE") == 0 ? false : true);
			} else if (strcmp(param->name(), "Policy") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "RAID0") == 0) {
					Policy = SSD_Array_Policy_Type::RAID0;
				} else if (strcmp(val.c_str(), "SWANS") == 0) {
					Policy = SSD_Array_Policy_Type::SWANS;
				} else {
					PRINT_ERROR("Unknown SSD array policy specified in the SSD configuration file! Supported values: RAID0, SWANS")
				}
			} else if (strcmp(param->name(), "SSD_No_In_Array") == 0) {
				std::string val = param->value();
				SSD_No_In_Array = std::stoul(val);
			} else if (strcmp(param->name(), "Zone_Size_MB") == 0) {
				std::string val = param->value();
				Zone_Size_MB = std::stoul(val);
			} else if (strcmp(param->name(), "Threshold_Precautionary_MB") == 0) {
				std::string val = param->value();
				Threshold_Precautionary_MB = std::stod(val);
			} else if (strcmp(param->name(), "Threshold_Critical_MB") == 0) {
				std::string val = param->value();
				Threshold_Critical_MB = std::stod(val);
			} else if (strcmp(param->name(), "Epoch_Default_ms") == 0) {
				std::string val = param->value();
				Epoch_Default_ms = std::stoull(val);
			} else if (strcmp(param->name(), "Epoch_Placement_ms") == 0) {
				std::string val = param->value();
				Epoch_Placement_ms = std::stoull(val);
			} else if (strcmp(param->name(), "Epoch_Migration_ms") == 0) {
				std::string val = param->value();
				Epoch_Migration_ms = std::stoull(val);
			}
		}
	} catch (...) {
		PRINT_ERROR("Error in SWANS_Parameter_Set!")
	}
}
