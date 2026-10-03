// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

// Executable reasoning tools, not production scheduling or Minecraft rules.
namespace circuit::exploration {
using Resource = std::uint32_t;
using Tick = std::uint64_t;

struct Access {
    std::set<Resource> reads;
    std::set<Resource> writes;
};
bool conflicts(const Access& a, const Access& b);
// Input order is an ALREADY ESTABLISHED semantic order. Reorder only independent
// actions. All effects (including logical/global resources) must be declared.
std::vector<std::vector<std::size_t>> dependency_waves(std::span<const Access> actions);

struct EventKey {
    Tick tick;
    std::uint32_t phase;
    std::uint64_t order;
    auto operator<=>(const EventKey&) const = default;
};
struct Event {
    EventKey key;
    Resource target;
    std::int64_t value;
};
// Coordinator-only agenda. It does not prove that all producers have finished.
class Agenda {
public:
    void schedule(Event event);
    std::optional<Event> next(Tick through_tick);
    std::size_t size() const { return events_.size(); }
private:
    std::map<EventKey, Event> events_;
    std::optional<EventKey> last_;
    Tick watermark_{};
    Tick horizon_{};
};

struct Observation {
    std::int64_t value{};
    std::uint64_t revision{};
    std::uint64_t generation{};
    bool operator==(const Observation&) const = default;
};
struct Proposal {
    std::map<Resource, Observation> expected;
    std::map<Resource, std::int64_t> writes;
    std::vector<std::string> events;
};
enum class Commit { applied, stale, undeclared_write, exhausted_revision };
// Coordinator-only optimistic model with copy-before-publish. A production
// implementation needs ownership reservations, operation IDs, and bounded state.
class State {
public:
    void define(Resource id, std::int64_t value);
    Observation observe(Resource id) const;
    void replace(Resource id, std::int64_t value);
    Commit commit(const Proposal& proposal);
    const std::vector<std::string>& events() const { return events_; }
private:
    std::map<Resource, Observation> cells_;
    std::vector<std::string> events_;
};

struct OwnerTicket { Resource owner; std::uint64_t generation; };
enum class Advance { advanced, already_advanced, stale_owner, past_tick };
class EntityClock {
public:
    explicit EntityClock(Resource owner) : owner_(owner) {}
    OwnerTicket ticket() const { return {owner_, generation_}; }
    Advance advance(OwnerTicket ticket, Tick tick);
    void migrate(Resource owner);
    std::uint64_t age() const { return age_; }
private:
    Resource owner_;
    std::uint64_t generation_{};
    std::optional<Tick> last_tick_;
    std::uint64_t age_{};
};

// All root producers must be registered up front. A live ticket must register
// child work BEFORE retiring. This is a serial accounting model, not a queue.
class StageLedger {
public:
    using Ticket = std::uint64_t;
    explicit StageLedger(std::uint32_t roots);
    Ticket fork(Ticket parent);
    void finish(Ticket ticket);
    bool closed() const { return live_.empty(); }
private:
    std::set<Ticket> live_;
    Ticket next_{};
};
}
