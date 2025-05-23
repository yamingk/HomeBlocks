
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
#include "volume.hpp"
#include <homestore/replication_service.hpp>
#include <iomgr/iomgr_flip.hpp>

namespace homeblocks {

// this API will be called by volume manager after volume sb is recovered and volume is created;
shared< VolumeIndexTable > Volume::init_index_table(bool is_recovery, shared< VolumeIndexTable > tbl) {
    if (!is_recovery) {
        index_cfg_t cfg(homestore::hs()->index_service().node_size());
        cfg.m_leaf_node_type = homestore::btree_node_type::PREFIX;
        cfg.m_int_node_type = homestore::btree_node_type::FIXED;

        // create index table;
        auto uuid = hb_utils::gen_random_uuid();

        // user_sb_size is not currently enabled in homestore;
        // parent uuid is used during recovery in homeblks layer;
        LOGI("Creating index table for volume: {}, index_uuid: {}, parent_uuid: {}", vol_info_->name,
             boost::uuids::to_string(uuid), boost::uuids::to_string(id()));
        indx_tbl_ = std::make_shared< VolumeIndexTable >(uuid, id() /*parent uuid*/, 0 /*user_sb_size*/, cfg);
    } else {
        indx_tbl_ = tbl;
    }

    homestore::hs()->index_service().add_index_table(indx_table());
    return indx_table();
}

Volume::Volume(sisl::byte_view const& buf, void* cookie) : sb_{VOL_META_NAME} {
    sb_.load(buf, cookie);
    // generate volume info from sb;
    vol_info_ = std::make_shared< VolumeInfo >(sb_->id, sb_->size, sb_->page_size, sb_->name);
    LOGI("Volume superblock loaded from disk, vol_info : {}", vol_info_->to_string());
}

bool Volume::init(bool is_recovery) {
    if (!is_recovery) {
        // first time creation of the Volume, let's write the superblock;

        // 0. create the superblock;
        sb_.create(sizeof(vol_sb_t));
        sb_->init(vol_info_->page_size, vol_info_->size_bytes, vol_info_->id, vol_info_->name);

        // 1. create solo repl dev for volume;
        // members left empty on purpose for solo repl dev
        LOGI("Creating solo repl dev for volume: {}, uuid: {}", vol_info_->name, boost::uuids::to_string(id()));
        auto ret = homestore::hs()->repl_service().create_repl_dev(id(), {} /*members*/).get();
        if (ret.hasError()) {
            LOGE("Failed to create solo repl dev for volume: {}, uuid: {}, error: {}", vol_info_->name,
                 boost::uuids::to_string(vol_info_->id), ret.error());

            return false;
        }
        rd_ = ret.value();

        // 2. create the index table;
        init_index_table(false /*is_recovery*/);

        // 3. mark state as online;
        state_change(vol_state::ONLINE);
    } else {
        // recovery path
        LOGI("Getting repl dev for volume: {}, uuid: {}", vol_info_->name, boost::uuids::to_string(id()));
        auto ret = homestore::hs()->repl_service().get_repl_dev(id());

        if (ret.hasError()) {
            LOGI("Volume in destroying state? Failed to get repl dev for volume name: {}, uuid: {}, error: {}",
                 vol_info_->name, boost::uuids::to_string(vol_info_->id), ret.error());
            rd_ = nullptr;
            // DEBUG_ASSERT(false, "Failed to get repl dev for volume");
            // return false;
        } else {
            rd_ = ret.value();
        }

        // index table will be recovered via in subsequent callback with init_index_table API;
    }
    return true;
}

void Volume::destroy() {
    // 0. Set destroying state in superblock;
    state_change(vol_state::DESTROYING);

    // 1. destroy the repl dev;
    if (rd_) {
        LOGI("Destroying repl dev for volume: {}, uuid: {}", vol_info_->name, boost::uuids::to_string(id()));
        homestore::hs()->repl_service().remove_repl_dev(id()).get();
        rd_ = nullptr;
    }

#ifdef _PRERELEASE
    if (iomgr_flip::instance()->test_flip("vol_destroy_crash_simulation")) {
        // this is to simulate crash during volume destroy;
        // volume should be able to resume destroy on next reboot;
        LOGINFO("Volume destroy crash simulation flip is set, aborting");
        return;
    }
#endif

    // 2. destroy the index table;
    if (indx_tbl_) {
        LOGI("Destroying index table for volume: {}, uuid: {}", vol_info_->name, boost::uuids::to_string(id()));
        homestore::hs()->index_service().remove_index_table(indx_tbl_);
        indx_tbl_->destroy();
        indx_tbl_ = nullptr;
    }

    // destroy the superblock which will remove sb from meta svc;
    sb_.destroy();
}

} // namespace homeblocks
