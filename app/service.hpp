#pragma once
#include "boot/iso.hpp"
#include "platform/windows/virtual_backend.hpp"

namespace diskboot {
struct Request {
    std::filesystem::path iso, lab_vhd;
    std::string           disk_id, partition_guid;
    Bytes                 size = 0;
};
Plan                  plan_request(const Request &request);
Json                  disk_inventory();
std::filesystem::path prepare_virtual(const Request &request, const Plan &approved,
                                      const std::function<void(const std::string &)> &progress);
Json                  transaction_status();
Json                  verify_transaction(const std::filesystem::path &journal);
} // namespace diskboot
