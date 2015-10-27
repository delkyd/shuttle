#include "resource_manager.h"

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <gflags/gflags.h>
#include "logging.h"
#include "common/tools_util.h"

DECLARE_int32(input_block_size);

namespace baidu {
namespace shuttle {

IdManager::IdManager(int n) {
    for (int i = 0; i < n; ++i) {
        IdItem* item = new IdItem();
        item->no = i;
        item->attempt = 0;
        item->status = kResPending;
        resource_pool_.push_back(item);
        pending_res_.push_back(item);
    }
}

IdManager::~IdManager() {
    MutexLock lock(&mu_);
    for (std::vector<IdItem*>::iterator it = resource_pool_.begin();
            it != resource_pool_.end(); ++it) {
        delete *it;
    }
}

IdItem* IdManager::GetItem() {
    MutexLock lock(&mu_);
    if (pending_res_.empty()) {
        return NULL;
    }
    IdItem* cur = pending_res_.front();
    cur->attempt ++;
    cur->status = kResAllocated;
    pending_res_.pop_front();
    return new IdItem(*cur);
}

IdItem* IdManager::GetCertainItem(int no) {
    size_t n = static_cast<size_t>(no);
    MutexLock lock(&mu_);
    if (n > resource_pool_.size()) {
        LOG(WARNING, "this resource is not valid for duplication: %d", no);
        return NULL;
    }
    IdItem* cur = resource_pool_[n];
    if (cur->status == kResAllocated) {
        cur->attempt ++;
        return new IdItem(*cur);
    }
    LOG(WARNING, "this resource has not been allocated: %d", no);
    return NULL;
}

void IdManager::ReturnBackItem(int no) {
    size_t n = static_cast<size_t>(no);
    MutexLock lock(&mu_);
    if (n > resource_pool_.size()) {
        LOG(WARNING, "this resource is not valid for returning: %d", no);
    }
    IdItem* cur = resource_pool_[n];
    if (cur->status == kResAllocated) {
        cur->status = kResPending;
        pending_res_.push_front(cur);
    } else {
        LOG(WARNING, "invalid resource: %d", no);
    }
}

bool IdManager::FinishItem(int no) {
    size_t n = static_cast<size_t>(no);
    MutexLock lock(&mu_);
    if (n > resource_pool_.size()) {
        LOG(WARNING, "this resource is not valid for finishing: %d", no);
        return false;
    }
    IdItem* cur = resource_pool_[n];
    if (cur->status == kResAllocated) {
        cur->status = kResDone;
        return true;
    }
    LOG(WARNING, "resource may have been finished: %d", no);
    return false;
}

IdItem* const IdManager::CheckCertainItem(int no) {
    size_t n = static_cast<size_t>(no);
    MutexLock lock(&mu_);
    if (n > resource_pool_.size()) {
        LOG(WARNING, "this resource is not valid for checking: %d", no);
        return NULL;
    }
    return resource_pool_[n];
}

ResourceManager::ResourceManager(const std::vector<std::string>& input_files,
                                 FileSystem::Param& param) {
    if (boost::starts_with(input_files[0], "hdfs://")) {
        std::string host;
        int port;
        ParseHdfsAddress(input_files[0], &host, &port, NULL);
        param["host"] = host;
        param["port"] = boost::lexical_cast<std::string>(port);
    }
    fs_ = FileSystem::CreateInfHdfs(param);
    std::vector<FileInfo> files;
    std::string path;
    for (std::vector<std::string>::const_iterator it = input_files.begin();
            it != input_files.end(); ++it) {
        if (boost::starts_with(input_files[0], "hdfs://")) {
            ParseHdfsAddress(*it, NULL, NULL, &path);
        } else {
            path = *it;
        }
        if (path.find('*') == std::string::npos) {
            fs_->List(path, &files);
        } else {
            fs_->Glob(path, &files);
        }
    }
    MutexLock lock(&mu_);
    int counter = resource_pool_.size();
    const int64_t block_size = FLAGS_input_block_size;
    for (std::vector<FileInfo>::iterator it = files.begin();
            it != files.end(); ++it) {
        int blocks = it->size / block_size;
        for (int i = 0; i < blocks; ++i) {
            ResourceItem* item = new ResourceItem();
            item->no = counter++;
            item->attempt = 0;
            item->input_file = it->name;
            item->offset = i * block_size;
            item->size = block_size;
            resource_pool_.push_back(item);
        }
        int rest = it->size - blocks * block_size;
        ResourceItem* item = new ResourceItem();
        item->no = counter++;
        item->attempt = 0;
        item->input_file = it->name;
        item->offset = blocks * block_size;
        item->size = rest;
        resource_pool_.push_back(item);
    }
    manager_ = new IdManager(resource_pool_.size());
}

ResourceManager::~ResourceManager() {
    MutexLock lock(&mu_);
    for (std::vector<ResourceItem*>::iterator it = resource_pool_.begin();
            it != resource_pool_.end(); ++it) {
        delete *it;
    }
    delete manager_;
    delete fs_;
}

ResourceItem* ResourceManager::GetItem() {
    IdItem* item = manager_->GetItem();
    if (item == NULL) {
        return NULL;
    }
    ResourceItem* resource = resource_pool_[item->no];
    resource->attempt = item->attempt;
    delete item;
    return new ResourceItem(*resource);
}

ResourceItem* ResourceManager::GetCertainItem(int no) {
    IdItem* item = manager_->GetCertainItem(no);
    if (item == NULL) {
        return NULL;
    }
    ResourceItem* resource = resource_pool_[no];
    resource->attempt = item->attempt;
    delete item;
    return new ResourceItem(*resource);
}

void ResourceManager::ReturnBackItem(int no) {
    manager_->ReturnBackItem(no);
}

bool ResourceManager::FinishItem(int no) {
    return manager_->FinishItem(no);
}

ResourceItem* const ResourceManager::CheckCertainItem(int no) {
    IdItem* const item = manager_->CheckCertainItem(no);
    return resource_pool_[item->no];
}

}
}

