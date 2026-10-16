#pragma once
#include "model.hpp"
#include <functional>

namespace diskboot {
class Backend {
public:
    virtual ~Backend()                                                                          = default;
    virtual Inventory inventory(bool resize_bounds)                                             = 0;
    virtual void      authorize(const Identity &identity)                                       = 0;
    virtual void      resize(const Disk &disk, const Operation &op)                             = 0;
    virtual void      create(const Disk &disk, const Operation &op)                             = 0;
    virtual void      format(const Disk &disk, const Partition &partition, const Operation &op) = 0;
};
using JournalSink = std::function<void(const Json &)>;
struct Execution {
    Disk expected;
    Json journal;
};
void      verify_layout(const Disk &current, const Disk &expected);
Execution execute_partitions(const Plan &plan, Backend &backend, const JournalSink &persist);
Disk      revalidate(Backend &backend, const Identity &identity, const Disk &expected);
} // namespace diskboot
