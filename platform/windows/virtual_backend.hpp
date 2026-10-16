#pragma once
#include "core/executor.hpp"
#include "storage.hpp"

namespace diskboot {
class VirtualBackend final : public Backend {
    Storage storage_;
    Handle  virtual_disk_;

public:
    explicit VirtualBackend(const std::filesystem::path &vhd);
    Inventory inventory(bool resize_bounds) override {
        return storage_.inventory(resize_bounds);
    }
    void authorize(const Identity &identity) override;
    void resize(const Disk &disk, const Operation &op) override {
        authorize(disk.identity);
        storage_.resize(disk, op);
    }
    void create(const Disk &disk, const Operation &op) override {
        authorize(disk.identity);
        storage_.create(disk, op);
    }
    void format(const Disk &disk, const Partition &p, const Operation &op) override {
        authorize(disk.identity);
        storage_.format(disk, p, op);
    }
    Storage &storage() {
        return storage_;
    }
};
}
