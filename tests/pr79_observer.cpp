// External observation driver. All simulator implementation files are unmodified.
// Reuse the original XML parsing and reporting functions.
#define main mqsim_original_main
#include "main.cpp"
#undef main
#include "ssd/NVM_PHY_ONFI.h"
#include "ssd/Stats.h"
#include "ssd/TSU_Priority_OutOfOrder.h"
#include "ssd/TSU_OutOfOrder.h"

// Read private queue state without changing headers, class layout, or scheduling.
// Explicit template instantiation permits naming a private member as an argument.
template<class Tag, typename Tag::type Member> struct ReadMember {
    friend typename Tag::type member(Tag) { return Member; }
};
struct PriorityQueueTag {
    typedef SSD_Components::Flash_Transaction_Queue** SSD_Components::TSU_Priority_OutOfOrder::* type;
    friend type member(PriorityQueueTag);
};
struct OooQueueTag {
    typedef SSD_Components::Flash_Transaction_Queue** SSD_Components::TSU_OutOfOrder::* type;
    friend type member(OooQueueTag);
};
template struct ReadMember<PriorityQueueTag, &SSD_Components::TSU_Priority_OutOfOrder::MappingWriteTRQueue>;
template struct ReadMember<OooQueueTag, &SSD_Components::TSU_OutOfOrder::MappingWriteTRQueue>;

static SSD_Components::Flash_Transaction_Queue** mapping_queues(SSD_Device& ssd)
{
    auto tsu = static_cast<SSD_Components::FTL*>(ssd.Firmware)->TSU;
    if (auto priority = dynamic_cast<SSD_Components::TSU_Priority_OutOfOrder*>(tsu))
        return priority->*member(PriorityQueueTag{});
    if (auto ooo = dynamic_cast<SSD_Components::TSU_OutOfOrder*>(tsu))
        return ooo->*member(OooQueueTag{});
    return nullptr;
}

static unsigned long long mapping_read_completed = 0;
static unsigned long long mapping_write_completed = 0;
static unsigned long long user_read_completed = 0;
static unsigned long long user_write_completed = 0;

struct WlThresholdTag {
    typedef unsigned int SSD_Components::GC_and_WL_Unit_Base::* type;
    friend type member(WlThresholdTag);
};
template struct ReadMember<WlThresholdTag, &SSD_Components::GC_and_WL_Unit_Base::static_wearleveling_threshold>;
struct StaticWlTag {
    typedef bool SSD_Components::GC_and_WL_Unit_Base::* type;
    friend type member(StaticWlTag);
};
template struct ReadMember<StaticWlTag, &SSD_Components::GC_and_WL_Unit_Base::static_wearleveling_enabled>;

static void observe_completion(SSD_Components::NVM_Transaction_Flash* tr)
{
    using namespace SSD_Components;
    if (tr->Source == Transaction_Source_Type::MAPPING) {
        if (tr->Type == Transaction_Type::READ) ++mapping_read_completed;
        if (tr->Type == Transaction_Type::WRITE) ++mapping_write_completed;
    }
    if (tr->Source == Transaction_Source_Type::USERIO) {
        if (tr->Type == Transaction_Type::READ) ++user_read_completed;
        if (tr->Type == Transaction_Type::WRITE) ++user_write_completed;
    }
}

int main(int argc, char* argv[])
{
    if (argc != 5) return 1;
    std::string config, workload;
    command_line_args(argv, config, workload);
    Execution_Parameter_Set params;
    read_configuration_parameters(config, &params);
    auto scenarios = read_workload_definitions(workload);
    if (scenarios->size() != 1) return 2;
    Simulator->Reset();
    params.Host_Configuration.IO_Flow_Definitions = *scenarios->at(0);
    SSD_Device ssd(&params.SSD_Device_Configuration, &params.Host_Configuration.IO_Flow_Definitions);
    auto gcwl = static_cast<SSD_Components::FTL*>(ssd.Firmware)->GC_and_WL_Unit;
    if (gcwl->Use_dynamic_wearleveling() != params.SSD_Device_Configuration.Dynamic_Wearleveling_Enabled
        || gcwl->*member(StaticWlTag{}) != params.SSD_Device_Configuration.Static_Wearleveling_Enabled
        || gcwl->*member(WlThresholdTag{}) != params.SSD_Device_Configuration.Static_Wearleveling_Threshold) return 10;
    std::cout << "OBS settings_match=1 dynamic=" << gcwl->Use_dynamic_wearleveling()
              << " static=" << gcwl->*member(StaticWlTag{})
              << " threshold=" << gcwl->*member(WlThresholdTag{}) << std::endl;
    params.Host_Configuration.Input_file_path = workload.substr(0, workload.find_last_of('.'));
    Host_System host(&params.Host_Configuration, params.SSD_Device_Configuration.Enabled_Preconditioning, ssd.Host_interface);
    host.Attach_ssd_device(&ssd);
    // Read-only callback; attach before simulator Setup_triggers, so callbacks that
    // consume transactions cannot invalidate the pointer before observation.
    auto phy = static_cast<SSD_Components::NVM_PHY_ONFI*>(ssd.PHY);
    phy->ConnectToTransactionServicedSignal(observe_completion);
    Simulator->Start_simulation();
    std::cout << "OBS completion mapping_read=" << mapping_read_completed
              << " mapping_write=" << mapping_write_completed
              << " user_read=" << user_read_completed
              << " user_write=" << user_write_completed << std::endl;
    std::cout << "OBS generated mapping_write=" << SSD_Components::Stats::Total_flash_writes_for_mapping
              << " gc=" << SSD_Components::Stats::Total_gc_executions
              << " wl=" << SSD_Components::Stats::Total_wl_executions
              << " sim_time_ns=" << Simulator->Time() << std::endl;
    for (unsigned int ch = 0; ch < ssd.Channel_count; ++ch) {
        for (unsigned int chip = 0; chip < ssd.Chip_no_per_channel; ++chip) {
            std::cout << "OBS idle channel=" << ch << " chip=" << chip
                      << " channel_idle=" << (phy->Get_channel_status(ch) == SSD_Components::BusChannelStatus::IDLE)
                      << " chip_idle=" << (phy->GetChipStatus(phy->Get_chip(ch, chip)) == SSD_Components::ChipStatus::IDLE)
                      << std::endl;
        }
    }
    auto queues = mapping_queues(ssd);
    unsigned long long residual = 0, ready = 0;
    for (unsigned int ch = 0; ch < ssd.Channel_count; ++ch) {
        for (unsigned int chip = 0; chip < ssd.Chip_no_per_channel; ++chip) {
            residual += queues[ch][chip].size();
            for (auto tr : queues[ch][chip]) {
                if (static_cast<SSD_Components::NVM_Transaction_Flash_WR*>(tr)->RelatedRead == nullptr) ++ready;
            }
        }
    }
    std::cout << "OBS queue residual=" << residual << " ready=" << ready
              << " nonnull_read_dependency=" << (residual - ready) << std::endl;
    collect_results(ssd, host, (params.Host_Configuration.Input_file_path + "_scenario_1.xml").c_str());
#ifdef ISSUE004_READY_PROBE
    // Separate synthetic scheduler probe, AFTER the natural workload and results.
    // A ready mapping write with no read dependency is submitted on the idle chip.
    // No actual data is changed by this test: the broken scheduler leaves it queued.
    using namespace SSD_Components;
    auto tsu = static_cast<FTL*>(ssd.Firmware)->TSU;
    NVM::FlashMemory::Physical_Page_Address address(0, 0, 0, 0, 0, 0);
    auto probe = new NVM_Transaction_Flash_WR(Transaction_Source_Type::MAPPING,
        0, 8192, 0, 0, address, nullptr, 0, nullptr, 65535, 0);
    auto before = queues[0][0].size();
    tsu->Prepare_for_transaction_submit();
    tsu->Submit_transaction(probe);
    tsu->Schedule();
    bool retained = std::find(queues[0][0].begin(), queues[0][0].end(), probe) != queues[0][0].end();
    std::cout << "OBS ready_probe submitted=1 related_read_null=1 retained=" << retained
              << " before=" << before << " after=" << queues[0][0].size()
              << " chip_idle=" << (phy->GetChipStatus(phy->Get_chip(0, 0)) == ChipStatus::IDLE)
              << " channel_idle=" << (phy->Get_channel_status(0) == BusChannelStatus::IDLE) << std::endl;
    if (!retained || queues[0][0].size() != before + 1) return 3;
#endif
    return 0;
}
