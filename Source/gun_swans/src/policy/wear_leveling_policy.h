#ifndef RAID_SWANS_WEAR_LEVELING_POLICY_H
#define RAID_SWANS_WEAR_LEVELING_POLICY_H

#include <vector>
#include "swans_policy_types.h"
#include "zone_directory.h"

namespace RAID_Policy {

class WearLevelingPolicy
{
public:
	WearLevelingPolicy();

	void Initialize(unsigned int ssd_count,
		double th_precautionary,
		double th_critical,
		unsigned int max_concurrent_migrations);

	void Observe_host_write(unsigned int ssd_id, unsigned int write_sectors);
	void Observe_migration_write(unsigned int ssd_id, unsigned int write_sectors);
	void Transfer_writes(unsigned int from_ssd, unsigned int to_ssd, uint64_t write_count);
	PolicyDecision Evaluate(const ZoneDirectory& directory);
	bool Has_epoch_writes() const;

	double Last_mu() const { return last_mu; }
	PolicyState Current_state() const { return current_state; }
	uint64_t Host_write_sectors(unsigned int ssd_id) const;
	uint64_t Migration_write_sectors(unsigned int ssd_id) const;
	uint64_t Cumulative_write_sectors(unsigned int ssd_id) const;
	uint64_t Total_host_write_sectors() const;
	uint64_t Total_migration_write_sectors() const;
	uint64_t Total_cumulative_write_sectors() const;

private:
	unsigned int Pick_hottest_ssd() const;
	unsigned int Pick_coldest_ssd() const;
	double Compute_stddev() const;
	void Reset_epoch();

	bool initialized;
	unsigned int ssd_count;
	double th_precautionary;
	double th_critical;
	unsigned int max_concurrent_migrations;
	std::vector<uint64_t> epoch_writes;	// recent physical write sectors for event scheduling
	std::vector<uint64_t> placement_writes;	// cumulative physical write sectors used for hot/cold selection
	std::vector<uint64_t> observed_host_writes;	// redirected host write sectors; never decremented
	std::vector<uint64_t> observed_migration_writes;	// migration restore write sectors; never decremented
	double last_mu;
	PolicyState current_state;
};

} // namespace RAID_Policy

#endif // RAID_SWANS_WEAR_LEVELING_POLICY_H
