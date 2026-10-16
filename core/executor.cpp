#include "executor.hpp"
#include <algorithm>

namespace diskboot {
void verify_layout(const Disk &current, const Disk &expected) {
    require(current.identity == expected.identity && current.usable == expected.usable && !current.boot_chain &&
                current.gpt && current.healthy && current.online && current.writable &&
                current.partitions.size() == expected.partitions.size(),
            "Disk state differs from journal; further writes refused");
    for (const auto &p : expected.partitions) {
        auto it = std::ranges::find(current.partitions, p.guid, &Partition::guid);
        require(it != current.partitions.end() && it->type == p.type && it->range == p.range &&
                    it->attributes == p.attributes && it->filesystem == p.filesystem && it->label == p.label &&
                    !it->boot && !it->system && it->writable == p.writable,
                "Partition state differs from journal; further writes refused");
    }
}
Disk revalidate(Backend &backend, const Identity &identity, const Disk &expected) {
    backend.authorize(identity);
    const auto inventory = backend.inventory(false);
    require(inventory.protection_complete, "Windows protection evidence became incomplete");
    auto current = resolve(inventory, identity);
    verify_layout(current, expected);
    return current;
}
Execution execute_partitions(const Plan &plan, Backend &backend, const JournalSink &persist) {
    validate_plan(plan);
    auto        inventory = backend.inventory(true);
    const auto &initial   = resolve(inventory, plan.target);
    check_target(inventory, initial, true);
    verify_layout(initial, plan.original);
    const auto source     = plan.operations.front().kind == "resize" ? plan.operations.front().partition_guid : "";
    auto       recomputed = make_plan(inventory, plan.target, plan.iso, plan.allocation.size, source, true);
    require(Json(recomputed.operations) == Json(plan.operations) && recomputed.allocation == plan.allocation &&
                recomputed.linux_space == plan.linux_space,
            "Approved plan no longer matches current state");
    backend.authorize(plan.target);
    Execution state{
        initial,
        {{"schema", 1}, {"plan", plan}, {"phase", "validated"}, {"expected", initial}, {"completed", Json::array()}}};
    persist(state.journal);
    auto checkpoint = [&](const std::string &phase) {
        state.journal["phase"]    = phase;
        state.journal["expected"] = state.expected;
        persist(state.journal);
    };
    for (const auto &op : plan.operations) {
        auto current             = revalidate(backend, plan.target, state.expected);
        state.journal["pending"] = op;
        checkpoint(op.kind + "-intent");
        if (op.kind == "resize") {
            backend.resize(current, op);
            auto p   = std::ranges::find(state.expected.partitions, op.partition_guid, &Partition::guid);
            p->range = op.range;
            revalidate(backend, plan.target, state.expected);
        } else {
            for (const auto &p : current.partitions)
                require(!p.range.overlaps(op.range), "Creation overlaps existing partition");
            backend.create(current, op);
            backend.authorize(plan.target);
            inventory = backend.inventory(false);
            require(inventory.protection_complete, "Protection evidence lost after creation");
            auto             after   = resolve(inventory, plan.target);
            const Partition *created = nullptr;
            for (const auto &p : after.partitions)
                if (p.range == op.range && p.type == op.type) {
                    require(!created && !p.guid.empty() &&
                                std::ranges::none_of(state.expected.partitions,
                                                     [&](const auto &old) { return old.guid == p.guid; }),
                            "Ambiguous created partition");
                    created = &p;
                }
            require(created && (created->filesystem.empty() || created->filesystem == "RAW"),
                    "New partition missing or unexpectedly formatted");
            state.expected.partitions.push_back(*created);
            verify_layout(after, state.expected);
            checkpoint("create-verified");
            auto new_partition = *created;
            current            = revalidate(backend, plan.target, state.expected);
            checkpoint("format-intent");
            backend.format(current, new_partition, op);
            auto p        = std::ranges::find(state.expected.partitions, new_partition.guid, &Partition::guid);
            p->filesystem = op.filesystem;
            p->label      = op.label;
            revalidate(backend, plan.target, state.expected);
        }
        state.journal["completed"].push_back(op);
        state.journal.erase("pending");
        checkpoint(op.kind + "-verified");
    }
    checkpoint("partitions-verified");
    return state;
}
}
