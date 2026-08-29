#ifndef ASCII_TRACE_DEFINITION_H
#define ASCII_TRACE_DEFINITION_H

enum class Trace_Time_Unit { PICOSECOND, NANOSECOND, MICROSECOND};//The unit of arrival times in the input file

/* How many times a trace-based flow replays its trace file:
* - FIXED_COUNT: replay exactly Relay_Count times (Relay_Count == 1 means "play once", the default).
* - UNTIL_END_OF_LIFE: ignore Relay_Count and Percentage_To_Be_Executed, and keep replaying the
*   trace file until any flash back-end in the simulation (the single SSD, or any member SSD of a
*   RAID/SWANS array) reports that it has reached its end of life (see Device_Lifecycle_Monitor and
*   Flash_Block_Manager_Base::Has_reached_end_of_life). This is the "repeat until EOL" workload mode.*/
enum class Trace_Replay_Mode { FIXED_COUNT, UNTIL_END_OF_LIFE };
#define PicoSecondCoeff  1000000000000	//the coefficient to convert picoseconds to second
#define NanoSecondCoeff  1000000000	//the coefficient to convert nanoseconds to second
#define MicroSecondCoeff  1000000	//the coefficient to convert microseconds to second
#define ASCIITraceTimeColumn 0
#define ASCIITraceDeviceColumn 1
#define ASCIITraceAddressColumn 2
#define ASCIITraceSizeColumn 3
#define ASCIITraceTypeColumn 4
#define ASCIITraceWriteCode "0"
#define ASCIITraceReadCode "1"
#define ASCIITraceWriteCodeInteger 0
#define ASCIITraceReadCodeInteger 1
#define ASCIILineDelimiter ' '
#define ASCIIItemsPerLine 5

//The on-disk layout of a trace-based I/O flow's input file.
enum class Trace_Format_Type {
	MQSIM_ASCII,	//MQSim's native ASCII format (see the ASCIITrace* definitions above)
	MSRC_CSV		//The MSR Cambridge / SNIA block-I/O CSV trace format, see below
};

/* The MSR Cambridge (MSRC) traces (D. Narayanan et al., "Write Off-Loading: Practical Power
* Management for Enterprise Storage", FAST 2008) are distributed as gzipped CSV files, one file
* per traced volume, with the following columns:
*   Timestamp,Hostname,DiskNumber,Type,Offset,Size,ResponseTime
* - Timestamp: the I/O issue time in "Windows FILETIME" units (100-nanosecond ticks since 1601-01-01)
* - Type: "Read" or "Write"
* - Offset, Size: in bytes, relative to the start of the traced volume
* - ResponseTime: the response time observed on the original system, in FILETIME ticks (not used by MQSim,
*   which computes its own timing model)
* MQSim expects the .csv.gz files to be decompressed (e.g., with gzip -d) before use; MQSim does not
* link a decompression library. An optional CSV header line is automatically detected and skipped.*/
#define MSRCTraceTimestampColumn 0
#define MSRCTraceHostnameColumn 1
#define MSRCTraceDiskNumberColumn 2
#define MSRCTraceTypeColumn 3
#define MSRCTraceOffsetColumn 4
#define MSRCTraceSizeColumn 5
#define MSRCTraceResponseTimeColumn 6
#define MSRCLineDelimiter ','
#define MSRCItemsPerLine 7
#define MSRC_FILETIME_TICK_TO_NS 100 //one Windows FILETIME tick equals 100 nanoseconds

#endif // !ASCII_TRACE_DEFINITION_H
