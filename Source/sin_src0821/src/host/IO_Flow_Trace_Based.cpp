#include <cctype>
#include "IO_Flow_Trace_Based.h"
#include "../utils/StringTools.h"
#include "ASCII_Trace_Definition.h"
#include "../utils/DistributionTypes.h"
#include "../ssd/Device_Lifecycle_Monitor.h"

namespace Host_Components
{
IO_Flow_Trace_Based::IO_Flow_Trace_Based(const sim_object_id_type &name, uint16_t flow_id, LHA_type start_lsa_on_device, LHA_type end_lsa_on_device, uint16_t io_queue_id,
										 uint16_t nvme_submission_queue_size, uint16_t nvme_completion_queue_size, IO_Flow_Priority_Class::Priority priority_class, double initial_occupancy_ratio,
										 std::string trace_file_path, Trace_Format_Type trace_format, Trace_Time_Unit time_unit, unsigned int total_replay_count, Trace_Replay_Mode relay_mode, unsigned int percentage_to_be_simulated,
										 HostInterface_Types SSD_device_type, PCIe_Root_Complex *pcie_root_complex, SATA_HBA *sata_hba,
										 bool enabled_logging, sim_time_type logging_period, std::string logging_file_path) : IO_Flow_Base(name, flow_id, start_lsa_on_device, end_lsa_on_device, io_queue_id, nvme_submission_queue_size, nvme_completion_queue_size, priority_class, 0, initial_occupancy_ratio, 0, SSD_device_type, pcie_root_complex, sata_hba, enabled_logging, logging_period, logging_file_path),
																															  trace_file_path(trace_file_path), trace_format(trace_format), time_unit(time_unit), total_replay_no(total_replay_count), relay_mode(relay_mode), percentage_to_be_simulated(percentage_to_be_simulated),
																															  total_requests_in_file(0), time_offset(0), msrc_time_base(0), msrc_time_base_initialized(false), current_record_valid(false)
{
	current_record.Arrival_time = 0;
	current_record.Start_LBA = 0;
	current_record.Size_in_sectors = 0;
	current_record.Is_write = false;

	if (percentage_to_be_simulated > 100)
	{
		percentage_to_be_simulated = 100;
		PRINT_MESSAGE("Bad value for percentage of trace file! It is set to 100 % ");
	}
}

IO_Flow_Trace_Based::~IO_Flow_Trace_Based()
{
}

//Parses one line already read from the trace file into a normalized Trace_Record, according to
//trace_format. Returns false if the line is malformed (MQSIM_ASCII) or is a header/malformed line
//that should be skipped (MSRC_CSV).
bool IO_Flow_Trace_Based::parse_line(const std::string& raw_line, Trace_Record& record)
{
	std::string line = raw_line;
	Utils::Helper_Functions::Remove_cr(line);
	std::vector<std::string> tokens;
	char *pEnd;

	switch (trace_format)
	{
	case Trace_Format_Type::MQSIM_ASCII:
	{
		Utils::Helper_Functions::Tokenize(line, ASCIILineDelimiter, tokens);
		if (tokens.size() != ASCIIItemsPerLine)
		{
			return false;
		}
		record.Arrival_time = std::strtoull(tokens[ASCIITraceTimeColumn].c_str(), &pEnd, 10);
		record.Start_LBA = std::strtoull(tokens[ASCIITraceAddressColumn].c_str(), &pEnd, 0);
		record.Size_in_sectors = std::strtoul(tokens[ASCIITraceSizeColumn].c_str(), &pEnd, 0);
		record.Is_write = (tokens[ASCIITraceTypeColumn].compare(ASCIITraceWriteCode) == 0);
		return true;
	}
	case Trace_Format_Type::MSRC_CSV:
	{
		Utils::Helper_Functions::Tokenize(line, MSRCLineDelimiter, tokens);
		if (tokens.size() != MSRCItemsPerLine || tokens[MSRCTraceTimestampColumn].empty())
		{
			return false;
		}
		//A CSV header line (e.g., "Timestamp,Hostname,...") has a non-numeric first field; strtoull
		//would silently return 0 for it, which must not be mistaken for a valid record at time 0.
		if (!isdigit((unsigned char)tokens[MSRCTraceTimestampColumn][0]))
		{
			return false;
		}
		uint64_t raw_timestamp = std::strtoull(tokens[MSRCTraceTimestampColumn].c_str(), &pEnd, 10);
		record.Arrival_time = (raw_timestamp - msrc_time_base) * MSRC_FILETIME_TICK_TO_NS;
		uint64_t offset_bytes = std::strtoull(tokens[MSRCTraceOffsetColumn].c_str(), &pEnd, 10);
		uint64_t size_bytes = std::strtoull(tokens[MSRCTraceSizeColumn].c_str(), &pEnd, 10);
		record.Start_LBA = offset_bytes / SECTOR_SIZE_IN_BYTE;
		record.Size_in_sectors = (unsigned int)((size_bytes + SECTOR_SIZE_IN_BYTE - 1) / SECTOR_SIZE_IN_BYTE);
		if (record.Size_in_sectors == 0)
		{
			record.Size_in_sectors = 1;
		}
		const std::string& type_field = tokens[MSRCTraceTypeColumn];
		record.Is_write = (type_field.size() > 0 && (type_field[0] == 'W' || type_field[0] == 'w'));
		return true;
	}
	default:
		return false;
	}
}

//Reads lines from trace_file until a valid record is parsed or EOF is reached. For MQSIM_ASCII, a
//malformed line terminates the trace (matching MQSim's original ASCII-trace behavior). For MSRC_CSV,
//a malformed or header line is skipped so that an optional CSV header row does not end the trace.
bool IO_Flow_Trace_Based::read_next_record(Trace_Record& record)
{
	std::string line;
	while (std::getline(trace_file, line))
	{
		if (parse_line(line, record))
		{
			return true;
		}
		if (trace_format == Trace_Format_Type::MQSIM_ASCII)
		{
			return false;
		}
		//MSRC_CSV: skip the malformed/header line and keep reading
	}
	return false;
}

sim_time_type IO_Flow_Trace_Based::detect_msrc_time_base()
{
	std::ifstream f;
	f.open(trace_file_path, std::ios::in);
	if (!f.is_open())
	{
		PRINT_ERROR("Error while opening input trace file: " << trace_file_path)
	}

	std::string line;
	char *pEnd;
	while (std::getline(f, line))
	{
		Utils::Helper_Functions::Remove_cr(line);
		std::vector<std::string> tokens;
		Utils::Helper_Functions::Tokenize(line, MSRCLineDelimiter, tokens);
		if (tokens.size() == MSRCItemsPerLine && !tokens[MSRCTraceTimestampColumn].empty()
			&& isdigit((unsigned char)tokens[MSRCTraceTimestampColumn][0]))
		{
			f.close();
			return std::strtoull(tokens[MSRCTraceTimestampColumn].c_str(), &pEnd, 10);
		}
	}
	f.close();
	PRINT_ERROR("MSRC trace file contains no valid data records: " << trace_file_path)
	return 0;
}

void IO_Flow_Trace_Based::ensure_msrc_time_base()
{
	if (trace_format == Trace_Format_Type::MSRC_CSV && !msrc_time_base_initialized)
	{
		msrc_time_base = detect_msrc_time_base();
		msrc_time_base_initialized = true;
	}
}

Host_IO_Request *IO_Flow_Trace_Based::Generate_next_request()
{
	if (!current_record_valid || STAT_generated_request_count >= total_requests_to_be_generated)
	{
		return NULL;
	}

	Host_IO_Request *request = new Host_IO_Request;
	if (current_record.Is_write)
	{
		request->Type = Host_IO_Request_Type::WRITE;
		STAT_generated_write_request_count++;
	}
	else
	{
		request->Type = Host_IO_Request_Type::READ;
		STAT_generated_read_request_count++;
	}

	request->LBA_count = current_record.Size_in_sectors;

	request->Start_LBA = current_record.Start_LBA;
	if (request->Start_LBA <= (end_lsa_on_device - start_lsa_on_device))
	{
		request->Start_LBA += start_lsa_on_device;
	}
	else
	{
		request->Start_LBA = start_lsa_on_device + request->Start_LBA % (end_lsa_on_device - start_lsa_on_device);
	}

	request->Arrival_time = time_offset + Simulator->Time();
	STAT_generated_request_count++;

	return request;
}

void IO_Flow_Trace_Based::NVMe_consume_io_request(Completion_Queue_Entry *io_request)
{
	IO_Flow_Base::NVMe_consume_io_request(io_request);
	IO_Flow_Base::NVMe_update_and_submit_completion_queue_tail();
}

void IO_Flow_Trace_Based::SATA_consume_io_request(Host_IO_Request *io_request)
{
	IO_Flow_Base::SATA_consume_io_request(io_request);
}

void IO_Flow_Trace_Based::Start_simulation()
{
	IO_Flow_Base::Start_simulation();
	ensure_msrc_time_base();

	trace_file.open(trace_file_path, std::ios::in);
	if (!trace_file.is_open())
	{
		PRINT_ERROR("Error while opening input trace file: " << trace_file_path)
	}
	PRINT_MESSAGE("Investigating input trace file: " << trace_file_path);

	sim_time_type last_request_arrival_time = 0;
	Trace_Record record;
	while (read_next_record(record))
	{
		total_requests_in_file++;
		sim_time_type prev_time = last_request_arrival_time;
		last_request_arrival_time = record.Arrival_time;
		if (last_request_arrival_time < prev_time)
		{
			PRINT_ERROR("Unexpected request arrival time: " << last_request_arrival_time << "\nMQSim expects request arrival times to be monotonically increasing in the input trace!")
		}
	}

	trace_file.close();
	if (total_requests_in_file == 0)
	{
		PRINT_ERROR("Input trace file contains no valid records: " << trace_file_path)
	}
	PRINT_MESSAGE("Trace file: " << trace_file_path << " seems healthy");

	if (relay_mode == Trace_Replay_Mode::UNTIL_END_OF_LIFE)
	{
		//Relay_Count and Percentage_To_Be_Executed do not apply in this mode: the trace is replayed
		//repeatedly until Device_Lifecycle_Monitor reports that some device has reached its end of
		//life (checked at the top of every Execute_simulator_event call). The large sentinel below is
		//only a last-resort safety bound in case end of life is never reached (e.g., misconfiguration).
		total_requests_to_be_generated = 0xFFFFFFFFu;
		PRINT_MESSAGE("Flow " << ID() << " will replay its trace file until a device reaches end of life (Relay_Mode = UNTIL_END_OF_LIFE)")
	}
	else if (total_replay_no == 1)
	{
		total_requests_to_be_generated = (int)(((double)percentage_to_be_simulated / 100) * total_requests_in_file);
	}
	else
	{
		total_requests_to_be_generated = total_requests_in_file * total_replay_no;
	}

	trace_file.open(trace_file_path);
	current_record_valid = read_next_record(current_record);
	Simulator->Register_sim_event(current_record.Arrival_time, this);
}

void IO_Flow_Trace_Based::Validate_simulation_config()
{
}

void IO_Flow_Trace_Based::Execute_simulator_event(MQSimEngine::Sim_Event *)
{
	if (relay_mode == Trace_Replay_Mode::UNTIL_END_OF_LIFE && SSD_Components::Device_Lifecycle_Monitor::Has_reached_end_of_life())
	{
		//Some device (the single SSD, or a member SSD of a RAID/SWANS array) has reached end of
		//life: stop generating further requests from this flow. No further sim event is registered,
		//so this flow's replay loop naturally ends here.
		return;
	}

	Host_IO_Request *request = Generate_next_request();
	if (request != NULL)
	{
		Submit_io_request(request);
	}

	if (STAT_generated_request_count < total_requests_to_be_generated)
	{
		if (!read_next_record(current_record))
		{
			trace_file.close();
			trace_file.open(trace_file_path);
			replay_counter++;
			time_offset = Simulator->Time();
			current_record_valid = read_next_record(current_record);
			PRINT_MESSAGE("* Replay round " << replay_counter << "of " << total_replay_no << " started  for" << ID())
		}
		Simulator->Register_sim_event(time_offset + current_record.Arrival_time, this);
	}
}

void IO_Flow_Trace_Based::Get_statistics(Utils::Workload_Statistics &stats, LPA_type (*Convert_host_logical_address_to_device_address)(LHA_type lha),
										 page_status_type (*Find_NVM_subunit_access_bitmap)(LHA_type lha))
{
	stats.Type = Utils::Workload_Type::TRACE_BASED;
	stats.Stream_id = io_queue_id - 1; //In MQSim, there is a simple relation between stream id and the io_queue_id of NVMe
	stats.Min_LHA = start_lsa_on_device;
	stats.Max_LHA = end_lsa_on_device;
	for (int i = 0; i < MAX_ARRIVAL_TIME_HISTOGRAM + 1; i++)
	{
		stats.Write_arrival_time.push_back(0);
		stats.Read_arrival_time.push_back(0);
	}
	for (int i = 0; i < MAX_REQSIZE_HISTOGRAM_ITEMS + 1; i++)
	{
		stats.Write_size_histogram.push_back(0);
		stats.Read_size_histogram.push_back(0);
	}
	stats.Total_generated_requests = 0;
	stats.Total_accessed_lbas = 0;

	ensure_msrc_time_base();

	std::ifstream trace_file_temp;
	trace_file_temp.open(trace_file_path, std::ios::in);
	if (!trace_file_temp.is_open())
	{
		PRINT_ERROR("Error while opening the input trace file!")
	}

	std::string trace_line;
	sim_time_type last_request_arrival_time = 0;
	sim_time_type sum_inter_arrival = 0;
	uint64_t sum_request_size = 0;
	Trace_Record record;
	while (std::getline(trace_file_temp, trace_line))
	{
		if (!parse_line(trace_line, record))
		{
			if (trace_format == Trace_Format_Type::MQSIM_ASCII)
			{
				break;
			}
			continue; //MSRC_CSV: skip a header/malformed line
		}

		sim_time_type prev_time = last_request_arrival_time;
		last_request_arrival_time = record.Arrival_time;
		if (last_request_arrival_time < prev_time)
		{
			PRINT_ERROR("Unexpected request arrival time: " << last_request_arrival_time << "\nMQSim expects request arrival times to be monotonic increasing in the input trace!")
		}
		sim_time_type diff = (last_request_arrival_time - prev_time) / 1000; //The arrival rate histogram is stored in the microsecond unit
		sum_inter_arrival += last_request_arrival_time - prev_time;

		unsigned int LBA_count = record.Size_in_sectors;
		sum_request_size += LBA_count;
		LHA_type start_LBA = record.Start_LBA;
		if (start_LBA <= (end_lsa_on_device - start_lsa_on_device))
		{
			start_LBA += start_lsa_on_device;
		}
		else
		{
			start_LBA = start_lsa_on_device + start_LBA % (end_lsa_on_device - start_lsa_on_device);
		}
		LHA_type end_LBA = start_LBA + LBA_count - 1;
		if (end_LBA > end_lsa_on_device)
		{
			end_LBA = start_lsa_on_device + (end_LBA - end_lsa_on_device) - 1;
		}

		//Address access pattern statistics
		while (start_LBA <= end_LBA)
		{
			LPA_type device_address = Convert_host_logical_address_to_device_address(start_LBA);
			page_status_type access_status_bitmap = Find_NVM_subunit_access_bitmap(start_LBA);
			if (record.Is_write)
			{
				if (stats.Write_address_access_pattern.find(device_address) == stats.Write_address_access_pattern.end())
				{
					Utils::Address_Histogram_Unit hist;
					hist.Access_count = 1;
					hist.Accessed_sub_units = access_status_bitmap;
					stats.Write_address_access_pattern[device_address] = hist;
				}
				else
				{
					stats.Write_address_access_pattern[device_address].Access_count = stats.Write_address_access_pattern[device_address].Access_count + 1;
					stats.Write_address_access_pattern[device_address].Accessed_sub_units = stats.Write_address_access_pattern[device_address].Accessed_sub_units | access_status_bitmap;
				}

				if (stats.Read_address_access_pattern.find(device_address) != stats.Read_address_access_pattern.end())
				{
					stats.Write_read_shared_addresses.insert(device_address);
				}
			}
			else
			{
				if (stats.Read_address_access_pattern.find(device_address) == stats.Read_address_access_pattern.end())
				{
					Utils::Address_Histogram_Unit hist;
					hist.Access_count = 1;
					hist.Accessed_sub_units = access_status_bitmap;
					stats.Read_address_access_pattern[device_address] = hist;
				}
				else
				{
					stats.Read_address_access_pattern[device_address].Access_count = stats.Read_address_access_pattern[device_address].Access_count + 1;
					stats.Read_address_access_pattern[device_address].Accessed_sub_units = stats.Read_address_access_pattern[device_address].Accessed_sub_units | access_status_bitmap;
				}

				if (stats.Write_address_access_pattern.find(device_address) != stats.Write_address_access_pattern.end())
				{
					stats.Write_read_shared_addresses.insert(device_address);
				}
			}
			stats.Total_accessed_lbas++;
			start_LBA++;
			if (start_LBA > end_lsa_on_device)
			{
				start_LBA = start_lsa_on_device;
			}
		}

		//Request size statistics
		if (record.Is_write)
		{
			if (diff < MAX_ARRIVAL_TIME_HISTOGRAM)
			{
				stats.Write_arrival_time[diff]++;
			}
			else
			{
				stats.Write_arrival_time[MAX_ARRIVAL_TIME_HISTOGRAM]++;
			}

			if (LBA_count < MAX_REQSIZE_HISTOGRAM_ITEMS)
			{
				stats.Write_size_histogram[LBA_count]++;
			}
			else
			{
				stats.Write_size_histogram[MAX_REQSIZE_HISTOGRAM_ITEMS]++;
			}
		}
		else
		{
			if (diff < MAX_ARRIVAL_TIME_HISTOGRAM)
			{
				stats.Read_arrival_time[diff]++;
			}
			else
			{
				stats.Read_arrival_time[MAX_ARRIVAL_TIME_HISTOGRAM]++;
			}

			if (LBA_count < MAX_REQSIZE_HISTOGRAM_ITEMS)
			{
				stats.Read_size_histogram[LBA_count]++;
			}
			else
			{
				stats.Read_size_histogram[(unsigned int)MAX_REQSIZE_HISTOGRAM_ITEMS]++;
			}
		}
		stats.Total_generated_requests++;
	}
	trace_file_temp.close();
	stats.Average_request_size_sector = (unsigned int)(sum_request_size / stats.Total_generated_requests);
	stats.Average_inter_arrival_time_nano_sec = sum_inter_arrival / stats.Total_generated_requests;

	stats.Initial_occupancy_ratio = initial_occupancy_ratio;
	stats.Replay_no = total_replay_no;
}
} // namespace Host_Components
