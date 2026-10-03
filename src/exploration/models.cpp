// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/exploration/models.hpp>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace circuit::exploration {
namespace {
bool intersects(const std::set<Resource>& a, const std::set<Resource>& b) {
    return std::any_of(a.begin(), a.end(), [&](Resource id) { return b.contains(id); });
}
}
bool conflicts(const Access& a, const Access& b) {
    return intersects(a.writes, b.reads) || intersects(a.reads, b.writes) ||
           intersects(a.writes, b.writes);
}
std::vector<std::vector<std::size_t>> dependency_waves(std::span<const Access> actions) {
    std::vector<std::size_t> level(actions.size(), 0);
    std::vector<std::vector<std::size_t>> waves;
    for (std::size_t i = 0; i < actions.size(); ++i) {
        for (std::size_t earlier = 0; earlier < i; ++earlier)
            if (conflicts(actions[earlier], actions[i]))
                level[i] = std::max(level[i], level[earlier] + 1);
        if (waves.size() <= level[i]) waves.resize(level[i] + 1);
        waves[level[i]].push_back(i);
    }
    return waves;
}
void Agenda::schedule(Event event) {
    if (event.key.tick < watermark_ || (last_ && event.key <= *last_))
        throw std::invalid_argument("late event: semantic order has already advanced");
    if (!events_.emplace(event.key, event).second)
        throw std::invalid_argument("duplicate semantic event key");
}
std::optional<Event> Agenda::next(Tick through_tick) {
    if (through_tick < horizon_) throw std::invalid_argument("time cannot move backwards");
    horizon_ = through_tick;
    if (events_.empty() || events_.begin()->first.tick > through_tick) {
        watermark_ = through_tick;
        return std::nullopt;
    }
    auto event = events_.begin()->second;
    events_.erase(events_.begin());
    last_ = event.key;
    return event;
}
void State::define(Resource id, std::int64_t value) {
    if (!cells_.emplace(id, Observation{value, 0, 0}).second)
        throw std::invalid_argument("resource already defined");
}
Observation State::observe(Resource id) const { return cells_.at(id); }
void State::replace(Resource id, std::int64_t value) {
    auto& cell = cells_.at(id);
    if (cell.generation == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("generation exhausted");
    cell = {value, 0, cell.generation + 1};
}
Commit State::commit(const Proposal& proposal) {
    for (const auto& [id, expected] : proposal.expected) {
        auto found = cells_.find(id);
        if (found == cells_.end() || found->second != expected) return Commit::stale;
    }
    for (const auto& [id, value] : proposal.writes) {
        (void)value;
        if (!proposal.expected.contains(id)) return Commit::undeclared_write;
        if (cells_.at(id).revision == std::numeric_limits<std::uint64_t>::max())
            return Commit::exhausted_revision;
    }
    auto next_cells = cells_;
    auto next_events = events_;
    for (const auto& [id, value] : proposal.writes) {
        next_cells.at(id).value = value;
        ++next_cells.at(id).revision;
    }
    next_events.insert(next_events.end(), proposal.events.begin(), proposal.events.end());
    cells_.swap(next_cells);
    events_.swap(next_events);
    return Commit::applied;
}
Advance EntityClock::advance(OwnerTicket ticket, Tick tick) {
    if (ticket.owner != owner_ || ticket.generation != generation_) return Advance::stale_owner;
    if (last_tick_ && tick < *last_tick_) return Advance::past_tick;
    if (last_tick_ && tick == *last_tick_) return Advance::already_advanced;
    if (age_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("age exhausted");
    last_tick_ = tick;
    ++age_;
    return Advance::advanced;
}
void EntityClock::migrate(Resource owner) {
    if (owner == owner_) return;
    if (generation_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("generation exhausted");
    owner_ = owner;
    ++generation_;
}
StageLedger::StageLedger(std::uint32_t roots) {
    for (std::uint32_t i = 0; i < roots; ++i) live_.insert(next_++);
}
StageLedger::Ticket StageLedger::fork(Ticket parent) {
    if (!live_.contains(parent)) throw std::invalid_argument("parent ticket is no longer live");
    if (next_ == std::numeric_limits<Ticket>::max()) throw std::overflow_error("ticket IDs exhausted");
    const auto child = next_;
    live_.insert(child);
    ++next_;
    return child;
}
void StageLedger::finish(Ticket ticket) {
    if (live_.erase(ticket) != 1) throw std::invalid_argument("unknown or already completed ticket");
}
}
