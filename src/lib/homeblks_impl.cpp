
/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#include <algorithm>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/nil_generator.hpp>
#include <iomgr/io_environment.hpp>
#include <homestore/homestore.hpp>
#include <homestore/replication_service.hpp>
#include <sisl/options/options.h>
#include "homeblks_impl.hpp"
#include "listener.hpp"
#include "home_blks_config.hpp"

SISL_OPTION_GROUP(homeblocks,
                  (executor_type, "", "executor", "Executor to use for Future deferal",
                   ::cxxopts::value< std::string >()->default_value("immediate"), "immediate|cpu|io"));

SISL_LOGGING_DEF(HOMEBLOCKS_LOG_MODS)

namespace homeblocks {

extern std::shared_ptr< HomeBlocks > init_homeblocks(std::weak_ptr< HomeBlocksApplication >&& application) {
    LOGI("Initializing HomeBlocks with reaper thread timer: {} seconds", HB_DYNAMIC_CONFIG(reaper_thread_timer_secs));
    auto inst = std::make_shared< HomeBlocksImpl >(std::move(application));
    inst->init_homestore();
    inst->init_cp();
    inst->start_reaper_thread();
    return inst;
}

iomgr::drive_type HomeBlocksImpl::data_drive_type() const {
    auto const dtype = homestore::hs()->data_service().get_dev_type();
    if (dtype == homestore::HSDevType::Data) {
        return iomgr::drive_type::block_hdd;
    } else if (dtype == homestore::HSDevType::Fast) {
        return iomgr::drive_type::block_nvme;
    } else {
        return iomgr::drive_type::unknown;
    }
}

HomeBlocksStats HomeBlocksImpl::get_stats() const {
    auto const stats = homestore::hs()->repl_service().get_cap_stats();
    return {stats.total_capacity, stats.used_capacity};
}

folly::Future< folly::Unit > HomeBlocksImpl::shutdown_start() {
    LOGI("Setting shutdown start flag");
    shutdown_started_ = true;

    auto f = shutdown_promise_.getFuture();

    auto const nsecs = shutdown_timer_nsecs();
    LOGI("Setting shutdown timer with {} seconds", nsecs);
    // start timer thread if there are still outstanding jobs;
    shutdown_timer_hdl_ = iomanager.schedule_global_timer(
        nsecs * 1000 * 1000 * 1000, true /* recurring */, nullptr /* cookie */, iomgr::reactor_regex::all_user,
        [this](void*) { this->do_shutdown(); }, true /* wait_to_schedule */);

    return f;
}

uint64_t HomeBlocksImpl::shutdown_timer_nsecs() const {
    if (SISL_OPTIONS.count("shutdown_timer_nsecs")) {
        auto const n = SISL_OPTIONS["shutdown_timer_nsecs"].as< uint32_t >();
        LOGINFO("Using shutdown_timer_nsecs option value: {}", n);
        return n;
    } else {
        return HB_DYNAMIC_CONFIG(shutdown_thread_timer_secs);
    }
}

bool HomeBlocksImpl::no_outstanding_vols() const {
    // check if there are no volumes with outstanding requests;
    std::shared_lock lg(vol_lock_);
    for (auto const& vol_pair : vol_map_) {
        auto const& vol = vol_pair.second;
        // 1. check if volume is under the process of being removed;
        if (vol->is_destroying()) {
#ifdef _PRERELEASE
            if (crash_simulated_) {
                LOGI("Skipping volume {} under destruction as crash simulation is enabled", vol->id_str());
                continue; // skip volumes that are being removed due to crash simulation
            }
#endif
            LOGI("Found outstanding volume {} that is under destruction.", vol->id_str());
            return false;
        }

        // 2. if volume is not under destruction, check if it has outstanding requests;
        if (vol->num_outstanding_reqs() > 0) {
            LOGI("Found outstanding volume {} that has outstanding requests: {}", vol->id_str(),
                 vol->num_outstanding_reqs());
            return false; // found a volume with outstanding requests
        }
    }

    LOGI("No volumes with outstanding requests");
    return true; // no volumes with outstanding requests
}

bool HomeBlocksImpl::can_shutdown() const {
    // check if shutdown has started and no outstanding requests;
    if (is_shutting_down() && no_outstanding_vols() && outstanding_reqs_.test_eq(0)) {
        LOGI("Shutdown can proceed, outstanding requests: {}", outstanding_reqs_.get());
        return true;
    }

    LOGI("Shutdown cannot proceed, outstanding requests: {}", outstanding_reqs_.get());
    return false;
}

void HomeBlocksImpl::do_shutdown() {
    LOGI("Shutdown timer triggered, checking for outstanding requests");
    if (can_shutdown()) {
        LOGI("No outstanding requests, proceeding with shutdown");

        shutdown_promise_.setValue();
    } else {
        LOGI("Outstanding requests exist, will retry shutdown in {} seconds", shutdown_timer_nsecs());
    }
}

HomeBlocksImpl::~HomeBlocksImpl() {
    LOGI("Shutting down HomeBlocksImpl Received");
    DEBUG_ASSERT(!is_shutting_down(), "Shutdown already started, cannot destruct HomeBlocksImpl again");
    // set the shutdown flag so that no new requests are accepted;
    // start timer thread if there are still outstanding jobs;
    auto f = shutdown_start();

    std::move(f).get();

    // stop the timer thread
    if (shutdown_timer_hdl_ != iomgr::null_timer_handle) {
        iomanager.cancel_timer(shutdown_timer_hdl_);
        shutdown_timer_hdl_ = iomgr::null_timer_handle;
    }

    // set the shutdown flag so that no new requests are accepted;
    sb_->set_flag(SB_FLAGS_GRACEFUL_SHUTDOWN);
    sb_.write();

    homestore::hs()->shutdown();
    homestore::HomeStore::reset_instance();
    iomanager.stop();
}

HomeBlocksImpl::HomeBlocksImpl(std::weak_ptr< HomeBlocksApplication >&& application) :
        _application(std::move(application)), sb_{HB_META_NAME} {
    auto exe_type = SISL_OPTIONS["executor"].as< std::string >();
    std::transform(exe_type.begin(), exe_type.end(), exe_type.begin(), ::tolower);

    if ("immediate" == exe_type) [[likely]]
        executor_ = &folly::QueuedImmediateExecutor::instance();
    else if ("io" == exe_type)
        executor_ = folly::getGlobalIOExecutor();
    else if ("cpu" == exe_type)
        executor_ = folly::getGlobalCPUExecutor();
    else
        RELEASE_ASSERT(false, "Unknown Folly Executor type: [{}]", exe_type);
    LOGI("initialized with [executor={}]", exe_type);
    ordinal_reserver_ = std::make_unique< sisl::IDReserver >(MAX_NUM_VOLUMES);
}

DevType HomeBlocksImpl::get_device_type(std::string const& devname) {
    const iomgr::drive_type dtype = iomgr::DriveInterface::get_drive_type(devname);
    if (dtype == iomgr::drive_type::block_hdd || dtype == iomgr::drive_type::file_on_hdd) { return DevType::HDD; }
    if (dtype == iomgr::drive_type::file_on_nvme || dtype == iomgr::drive_type::block_nvme) { return DevType::NVME; }
    return DevType::UNSUPPORTED;
}

// repl application to init homestore
class HBReplApp : public homestore::ReplApplication {
public:
    HBReplApp(homestore::repl_impl_type impl_type, bool tl_consistency, HomeBlocksImpl* hb,
              std::weak_ptr< HomeBlocksApplication > ho_app) :
            impl_type_(impl_type), tl_consistency_(tl_consistency), hb_(hb), ho_app_(ho_app) {}

    // TODO: make this override after the base class in homestore adds a virtual destructor
    virtual ~HBReplApp() = default;

    // overrides
    homestore::repl_impl_type get_impl_type() const override { return impl_type_; }

    bool need_timeline_consistency() const override { return tl_consistency_; }

    // this will be called by homestore when create_repl_dev is called;
    std::shared_ptr< homestore::ReplDevListener > create_repl_dev_listener(homestore::group_id_t group_id) override {
        return std::make_shared< HBListener >(hb_);
#if 0
        std::scoped_lock lock_guard(_repl_sm_map_lock);
        auto [it, inserted] = _repl_sm_map.emplace(group_id, nullptr);
        if (inserted) { it->second = std::make_shared< ReplicationStateMachine >(hb_); }
        return it->second;
#endif
    }

    void on_repl_devs_init_completed() override { hb_->on_init_complete(); }

    std::pair< std::string, uint16_t > lookup_peer(homestore::replica_id_t uuid) const override {
        // r1: should never come here;
        RELEASE_ASSERT(false, "Unexpected to be called.");
        return std::make_pair("", 0);
    }

    homestore::replica_id_t get_my_repl_id() const override { return hb_->our_uuid(); }

    void destroy_repl_dev_listener(homestore::group_id_t gid) override {
        LOGI("Destroying repl dev listener for group_id {}", boost::uuids::to_string(gid));
    }

private:
    homestore::repl_impl_type impl_type_;
    bool tl_consistency_; // indicates whether this application needs timeline consistency;
    HomeBlocksImpl* hb_;
    std::weak_ptr< HomeBlocksApplication > ho_app_;
#if 0
    std::map< homestore::group_id_t, std::shared_ptr< HBListener> > _repl_sm_map;
    std::mutex _repl_sm_map_lock;
#endif
};

void HomeBlocksImpl::get_dev_info(shared< HomeBlocksApplication > app, std::vector< homestore::dev_info >& dev_info,
                                  bool& has_data_dev, bool& has_fast_dev) {
    for (auto const& dev : app->devices()) {
        auto input_dev_type = dev.type;
        auto detected_type = get_device_type(dev.path.string());
        LOGD("Device {} detected as {}", dev.path.string(), detected_type);
        auto final_type = (dev.type == DevType::AUTO_DETECT) ? detected_type : input_dev_type;
        if (final_type == DevType::UNSUPPORTED) {
            LOGW("Device {} is not supported, skipping", dev.path.string());
            continue;
        }
        if (input_dev_type != DevType::AUTO_DETECT && detected_type != final_type) {
            LOGW("Device {} detected as {}, but input type is {}, using input type", dev.path.string(), detected_type,
                 input_dev_type);
        }
        auto hs_type = (final_type == DevType::HDD) ? homestore::HSDevType::Data : homestore::HSDevType::Fast;
        if (hs_type == homestore::HSDevType::Data) { has_data_dev = true; }
        if (hs_type == homestore::HSDevType::Fast) { has_fast_dev = true; }
        dev_info.emplace_back(std::filesystem::canonical(dev.path).string(), hs_type);
    }
}

uint64_t HomeBlocksImpl::_hs_chunk_size = HS_CHUNK_SIZE;
void HomeBlocksImpl::init_homestore() {
    auto app = _application.lock();
    RELEASE_ASSERT(app, "HomeObjectApplication lifetime unexpected!");

    LOGI("Starting iomgr with {} threads, spdk: {}", app->threads(), false);
    ioenvironment.with_iomgr(iomgr::iomgr_params{.num_threads = app->threads(), .is_spdk = app->spdk_mode()})
        .with_http_server();

    const uint64_t app_mem_size = app->app_mem_size() * 1024 * 1024 * 1024;
    LOGI("Initialize and start HomeStore with app_mem_size = {}", app_mem_size);

    std::vector< homestore::dev_info > device_info;
    bool has_data_dev{false}, has_fast_dev{false};
    get_dev_info(app, device_info, has_data_dev, has_fast_dev);

    RELEASE_ASSERT(device_info.size() != 0, "No supported devices found!");

    chunk_selector_ = std::make_shared< VolumeChunkSelector >(
        [this](uint64_t volume_ordinal, const std::vector< chunk_num_t >& chunk_ids) {
            update_vol_sb_cb(volume_ordinal, chunk_ids);
        });
    using namespace homestore;
    // Note: timeline_consistency doesn't matter as we are using solo repl dev;
    auto repl_app =
        std::make_shared< HBReplApp >(repl_impl_type::solo, false /*timeline_consistency*/, this, _application);
    bool need_format = homestore::hs()
                           ->with_index_service(std::make_unique< HBIndexSvcCB >(this))
                           .with_repl_data_service(repl_app, chunk_selector_) // custom volume chunk selector
                           .start(hs_input_params{.devices = device_info, .app_mem_size = app_mem_size},
                                  [this]() { register_metablk_cb(); });
    if (need_format) {
        auto ret = app->discover_svc_id(std::nullopt);
        DEBUG_ASSERT(ret.has_value(), "UUID should be generated by application.");
        our_uuid_ = ret.value();
        LOGINFO("We are starting for the first time on svc_id: [{}]. Formatting HomeStore. ",
                boost::uuids::to_string(our_uuid()));
        if (has_data_dev && has_fast_dev) {
            // NOTE: chunk_size, num_chunks only has to specify one, can be deduced from each other.
            homestore::hs()->format_and_start({
                {HS_SERVICE::META, hs_format_params{.dev_type = HSDevType::Fast, .size_pct = 9.0}},
                {HS_SERVICE::LOG,
                 hs_format_params{
                     .dev_type = HSDevType::Fast, .size_pct = 45.0, .num_chunks = 0, .chunk_size = 32 * Mi}},
                {HS_SERVICE::INDEX, hs_format_params{.dev_type = HSDevType::Fast, .size_pct = 45.0}},
                {HS_SERVICE::REPLICATION,
                 hs_format_params{.dev_type = HSDevType::Data,
                                  .size_pct = 95.0,
                                  .num_chunks = 0, // num_chunks will be deduced from chunk_size
                                  .chunk_size = _hs_chunk_size,
                                  .block_size = DATA_BLK_SIZE,
                                  .chunk_sel_type = chunk_selector_type_t::CUSTOM}},
            });
        } else {
            auto run_on_type = has_fast_dev ? homestore::HSDevType::Fast : homestore::HSDevType::Data;
            LOGD("Running with Single mode, all service on {}", run_on_type);
            homestore::hs()->format_and_start({
                {HS_SERVICE::META, hs_format_params{.dev_type = run_on_type, .size_pct = 5.0}},
                {HS_SERVICE::LOG,
                 hs_format_params{.dev_type = run_on_type, .size_pct = 10.0, .num_chunks = 0, .chunk_size = 32 * Mi}},
                {HS_SERVICE::INDEX, hs_format_params{.dev_type = run_on_type, .size_pct = 5.0}},
                {HS_SERVICE::REPLICATION,
                 hs_format_params{.dev_type = run_on_type,
                                  .size_pct = 75.0,
                                  .num_chunks = 0, // num_chunks will be deduced from chunk_size;
                                  .chunk_size = _hs_chunk_size,
                                  .block_size = DATA_BLK_SIZE,
                                  .chunk_sel_type = chunk_selector_type_t::CUSTOM}},
            });
        }
        // repl_app->on_repl_devs_init_completed();
        superblk_init();
    } else {
        // we are starting on an existing system;
        DEBUG_ASSERT(our_uuid() != boost::uuids::nil_uuid(), "UUID should be recovered from HB superblock!");
        // now callback to application to nofity the uuid so that we are treated as an existing system;
        app->discover_svc_id(our_uuid());
        LOGINFO("We are starting on [{}].", boost::uuids::to_string(our_uuid_));
    }

    recovery_done_ = true;
    LOGI("Initialize and start HomeStore is successfully");
}

void HomeBlocksImpl::superblk_init() {
    sb_.create(sizeof(homeblks_sb_t));
    sb_->magic = HB_SB_MAGIC;
    sb_->version = HB_SB_VER;
    sb_->boot_cnt = 0;
    sb_->init_flag(0);
    sb_->svc_id = our_uuid_;
    sb_.write();
}

void HomeBlocksImpl::on_hb_meta_blk_found(sisl::byte_view const& buf, void* cookie) {
    sb_.load(buf, cookie);
    // sb verification
    RELEASE_ASSERT_EQ(sb_->version, HB_SB_VER);
    RELEASE_ASSERT_EQ(sb_->magic, HB_SB_MAGIC);

    if (sb_->test_flag(SB_FLAGS_GRACEFUL_SHUTDOWN)) {
        // if it is a gracefuln shutdown, this flag should be set again in shutdown routine;
        sb_->clear_flag(SB_FLAGS_GRACEFUL_SHUTDOWN);
        LOGI("System was shutdown gracefully");
    } else {
        LOGI("System experienced sudden crash since last boot");
    }

    ++sb_->boot_cnt;

    our_uuid_ = sb_->svc_id;

    LOGI("HomeBlks superblock loaded, boot_cnt: {}, svc_id: {}", sb_->boot_cnt, boost::uuids::to_string(our_uuid_));

    // avoid doing sb meta blk write in callback which will cause deadlock;
    // the 1st CP should flush all dirty SB before taking traffic;
}

void HomeBlocksImpl::register_metablk_cb() {
    // register some callbacks for metadata recovery;
    using namespace homestore;

    // HomeBlks SB
    homestore::hs()->meta_service().register_handler(
        HB_META_NAME,
        [this](homestore::meta_blk* mblk, sisl::byte_view buf, size_t size) {
            on_hb_meta_blk_found(std::move(buf), voidptr_cast(mblk));
        },
        nullptr /*recovery_comp_cb*/, true /* do_crc */);
}

void HomeBlocksImpl::on_init_complete() {
    // this is called after HomeStore all recovery completed.
    // Add anything that needs to be done here.
    using namespace homestore;

    // Volume SB
    homestore::hs()->meta_service().register_handler(
        Volume::VOL_META_NAME,
        [this](homestore::meta_blk* mblk, sisl::byte_view buf, size_t size) {
            on_vol_meta_blk_found(std::move(buf), voidptr_cast(mblk));
        },
        nullptr /*recovery_comp_cb*/, true /* do_crc */,
        std::optional< meta_subtype_vec_t >({homestore::hs()->repl_service().get_meta_blk_name()}));

    homestore::hs()->meta_service().read_sub_sb(Volume::VOL_META_NAME);
}

void HomeBlocksImpl::init_cp() {}

uint64_t HomeBlocksImpl::gc_timer_nsecs() const {
    if (SISL_OPTIONS.count("gc_timer_nsecs")) {
        auto const n = SISL_OPTIONS["gc_timer_nsecs"].as< uint32_t >();
        LOGINFO("Using gc_timer_nsecs option value: {}", n);
        return n;
    } else {
        return HB_DYNAMIC_CONFIG(reaper_thread_timer_secs);
    }
}

void HomeBlocksImpl::start_reaper_thread() {
    auto const nsecs = gc_timer_nsecs();
    LOGI("Starting volume garbage collection timer with interval: {} seconds", nsecs);
    vol_gc_timer_hdl_ = iomanager.schedule_global_timer(
        nsecs * 1000 * 1000 * 1000, true /* recurring */, nullptr /* cookie */, iomgr::reactor_regex::all_user,
        [this](void*) { this->vol_gc(); }, true /* wait_to_schedule */);
}

void HomeBlocksImpl::vol_gc() {
    LOGI("Running volume garbage collection");
    // loop through every volume and call remove volume if volume's ref_cnt is zero;
    std::vector< VolumePtr > vols_to_remove;
    {
        auto lg = std::shared_lock(vol_lock_);
        for (auto& vol_pair : vol_map_) {
            auto& vol = vol_pair.second;
            LOGI("Checking volume with id: {}, is_destroying: {}, can_remove: {}, num_outstanding_reqs: {}",
                 vol->id_str(), vol->is_destroying(), vol->can_remove(), vol->num_outstanding_reqs());

            if (vol->is_destroying() && vol->can_remove()) {
                // 1. volume has been issued with removed command before
                // 2. no one has already started removing it
                // 3. volume is not in use anymore (ref_cnt == 0)
                vols_to_remove.push_back(vol);
            }
        }
    }

    for (auto& vol : vols_to_remove) {
        LOGI("Garbage Collecting removed volume with id: {}", vol->id_str());
        remove_volume(vol->id());
    }
}

} // namespace homeblocks
