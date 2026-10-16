#include "core/executor.hpp"
#include <algorithm>
#include <functional>
#include <iostream>

using namespace diskboot;
class MemoryBackend final : public Backend {
public:
    Inventory state;
    unsigned  writes = 0;
    bool      deny = false, corrupt = false, crash = false;
    MemoryBackend() {
        Disk d;
        d.identity = {"disk-guid", "unique", "serial", "device", "Virtual test", 32 * GiB, 512, 4096, 15};
        d.usable   = {MiB, 32 * GiB - 2 * MiB};
        d.gpt = d.online = d.healthy = d.writable = true;
        d.protected_disk = d.boot_chain = false;
        Partition p;
        p.guid         = "data-guid";
        p.type         = DataType;
        p.filesystem   = "NTFS";
        p.label        = "Data";
        p.range        = {MiB, 28 * GiB};
        p.minimum      = 8 * GiB;
        p.maximum      = 32 * GiB - 2 * MiB;
        p.resize_known = p.healthy = p.writable = p.encryption_clear = true;
        d.partitions.push_back(p);
        state = {true, {d}};
    }
    Inventory inventory(bool) override {
        return state;
    }
    void authorize(const Identity &) override {
        require(!deny, "Virtual target denied");
    }
    void resize(const Disk &, const Operation &op) override {
        ++writes;
        state.disks[0].partitions[0].range = op.range;
        if (corrupt)
            state.disks[0].partitions[0].range.size -= MiB;
        if (crash)
            throw Error("Interrupted mutation");
    }
    void create(const Disk &, const Operation &op) override {
        ++writes;
        Partition p;
        p.guid       = "new-" + std::to_string(writes);
        p.type       = op.type;
        p.range      = op.range;
        p.filesystem = "RAW";
        p.healthy = p.writable = true;
        state.disks[0].partitions.push_back(p);
    }
    void format(const Disk &, const Partition &partition, const Operation &op) override {
        ++writes;
        auto &partitions = state.disks[0].partitions;
        auto  p          = std::ranges::find(partitions, partition.guid, &Partition::guid);
        p->filesystem    = op.filesystem;
        p->label         = op.label;
    }
    Plan plan() {
        Iso image;
        image.path      = "test.iso";
        image.sha256    = std::string(64, 'a');
        image.size      = GiB;
        image.profile   = "ubuntu-casper-loopback";
        image.candidate = true;
        return make_plan(state, state.disks[0].identity, image, 16 * GiB, "data-guid", true);
    }
};
static void refuses(const std::function<void()> &f) {
    try {
        f();
    } catch (const Error &) {
        return;
    }
    throw Error("Expected refusal");
}
int main() {
    try {
        unsigned tests = 0;
        {
            MemoryBackend     backend;
            auto              plan = backend.plan();
            std::vector<Json> journal;
            auto              state = execute_partitions(plan, backend, [&](const auto &j) { journal.push_back(j); });
            require(backend.writes == 5 && state.journal.at("phase") == "partitions-verified" &&
                        journal.front().at("phase") == "validated",
                    "Incomplete transaction");
            verify_layout(backend.state.disks[0], state.expected);
            require(backend.state.disks[0].partitions[0].range.size == 12 * GiB, "Wrong source shrink");
            ++tests;
        }
        {
            MemoryBackend backend;
            auto          plan = backend.plan();
            refuses(
                [&] { execute_partitions(plan, backend, [](const auto &) { throw Error("Journal unavailable"); }); });
            require(!backend.writes, "Write occurred without journal");
            ++tests;
        }
        {
            MemoryBackend backend;
            auto          plan = backend.plan();
            backend.deny       = true;
            refuses([&] { execute_partitions(plan, backend, [](const auto &) {}); });
            require(!backend.writes, "Unproven virtual disk modified");
            ++tests;
        }
        {
            MemoryBackend backend;
            auto          plan = backend.plan();
            backend.corrupt    = true;
            Json journal;
            refuses([&] { execute_partitions(plan, backend, [&](const auto &j) { journal = j; }); });
            require(backend.writes == 1 && journal.at("phase") == "resize-intent",
                    "Continued after verification failure");
            ++tests;
        }
        {
            MemoryBackend backend;
            auto          plan = backend.plan();
            backend.crash      = true;
            Json journal;
            refuses([&] { execute_partitions(plan, backend, [&](const auto &j) { journal = j; }); });
            require(backend.writes == 1 && journal.at("phase") == "resize-intent", "Interrupted state not journaled");
            backend.crash = false;
            refuses([&] { execute_partitions(plan, backend, [](const auto &) {}); });
            require(backend.writes == 1, "Blind retry after interruption");
            ++tests;
        }
        {
            MemoryBackend backend;
            auto          plan                     = backend.plan();
            backend.state.disks[0].identity.serial = "replacement";
            refuses([&] { execute_partitions(plan, backend, [](const auto &) {}); });
            require(!backend.writes, "Identity change ignored");
            ++tests;
        }
        {
            MemoryBackend backend;
            auto          plan            = backend.plan();
            backend.state.disks[0].number = 67;
            auto result                   = execute_partitions(plan, backend, [](const auto &) {});
            require(result.expected.number == 67, "Renumbering failed");
            ++tests;
        }
        {
            MemoryBackend backend;
            auto          plan = backend.plan();
            refuses([&] {
                execute_partitions(plan, backend, [&](const auto &j) {
                    if (j.at("phase") == "resize-verified")
                        backend.state.disks[0].boot_chain = true;
                });
            });
            require(backend.writes == 1, "Boot-chain protection change ignored");
            ++tests;
        }
        {
            MemoryBackend backend;
            auto          plan                  = backend.plan();
            plan.operations.back().range.offset = MiB;
            refuses([&] { execute_partitions(plan, backend, [](const auto &) {}); });
            require(!backend.writes, "Out-of-range plan executed");
            ++tests;
        }
        std::cout << tests << " transaction tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
