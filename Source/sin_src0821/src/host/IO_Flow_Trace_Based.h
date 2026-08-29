#ifndef IO_FLOW_TRACE_BASED_H
#define IO_FLOW_TRACE_BASED_H

#include <string>
#include <iostream>
#include <fstream>
#include "IO_Flow_Base.h"
#include "ASCII_Trace_Definition.h"

namespace Host_Components
{
class IO_Flow_Trace_Based : public IO_Flow_Base
{
public:
	IO_Flow_Trace_Based(const sim_object_id_type &name, uint16_t flow_id, LHA_type start_lsa_on_device, LHA_type end_lsa_on_device, uint16_t io_queue_id,
						uint16_t nvme_submission_queue_size, uint16_t nvme_completion_queue_size, IO_Flow_Priority_Class::Priority priority_class, double initial_occupancy_ratio,
						std::string trace_file_path, Trace_Format_Type trace_format, Trace_Time_Unit time_unit, unsigned int total_replay_count, Trace_Replay_Mode relay_mode, unsigned int percentage_to_be_simulated,
						HostInterface_Types SSD_device_type, PCIe_Root_Complex *pcie_root_complex, SATA_HBA *sata_hba,
						bool enabled_logging, sim_time_type logging_period, std::string logging_file_path);
	~IO_Flow_Trace_Based();
	Host_IO_Request *Generate_next_request();
	void NVMe_consume_io_request(Completion_Queue_Entry *);
	void SATA_consume_io_request(Host_IO_Request *);
	void Start_simulation();
	void Validate_simulation_config();
	void Execute_simulator_event(MQSimEngine::Sim_Event *);
	void Get_statistics(Utils::Workload_Statistics &stats, LPA_type (*Convert_host_logical_address_to_device_address)(LHA_type lha),
						page_status_type (*Find_NVM_subunit_access_bitmap)(LHA_type lha));

private:
	/* A trace record normalized to MQSim's internal units (nanoseconds for time, sectors for LBA/size),
	*  regardless of the on-disk trace format. This lets the rest of the class (request generation,
	*  replay, and statistics collection) stay format-agnostic.*/
	struct Trace_Record
	{
		sim_time_type Arrival_time;//nanoseconds, relative to the first request in the trace file
		LHA_type Start_LBA;//in sectors, relative to the start of the trace's logical device
		unsigned int Size_in_sectors;
		bool Is_write;
	};

	Trace_Format_Type trace_format;
	Trace_Time_Unit time_unit;
	unsigned int percentage_to_be_simulated;
	std::string trace_file_path;
	std::ifstream trace_file;
	unsigned int total_replay_no, replay_counter;
	Trace_Replay_Mode relay_mode;
	unsigned int total_requests_in_file;
	sim_time_type time_offset;

	//Only used when trace_format == MSRC_CSV: MSRC timestamps are absolute Windows FILETIME values,
	//so they are normalized to start at zero by subtracting the first request's raw timestamp.
	sim_time_type msrc_time_base;
	bool msrc_time_base_initialized;

	bool current_record_valid;
	Trace_Record current_record;

	void ensure_msrc_time_base();//lazily computes msrc_time_base exactly once, regardless of call order
	sim_time_type detect_msrc_time_base();//scans the trace file for the first valid record's raw timestamp
	bool parse_line(const std::string& raw_line, Trace_Record& record);//parses one already-read line according to trace_format; returns false for a malformed or (MSRC) header line
	bool read_next_record(Trace_Record& record);//reads lines from trace_file until a valid record is parsed or EOF is reached
};
} // namespace Host_Components

#endif // !IO_FLOW_TRACE_BASED_H
