/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <vrt/graph/device/fpga/control_lowering.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace vrt::graph::fpga {

namespace {

unsigned scalarBitWidth(ScalarType type) {
    switch (type) {
        case ScalarType::U8:
        case ScalarType::I8:
            return 8;
        case ScalarType::U16:
        case ScalarType::I16:
            return 16;
        case ScalarType::U32:
        case ScalarType::I32:
        case ScalarType::F32:
            return 32;
        case ScalarType::U64:
        case ScalarType::I64:
        case ScalarType::F64:
            return 64;
    }
    return 64;
}

bool isOrderingCompare(CompareOp op) {
    switch (op) {
        case CompareOp::LT:
        case CompareOp::LE:
        case CompareOp::GT:
        case CompareOp::GE:
            return true;
        default:
            return false;
    }
}

/// Flip a comparison operator when the operands are swapped
/// (`a OP b`  <=>  `b FLIP(OP) a`).
CompareOp flipOperands(CompareOp op) {
    switch (op) {
        case CompareOp::LT: return CompareOp::GT;
        case CompareOp::LE: return CompareOp::GE;
        case CompareOp::GT: return CompareOp::LT;
        case CompareOp::GE: return CompareOp::LE;
        default:            return op;  // EQ/NE are symmetric
    }
}

/// Identify the (scalar, constant) operand split.  Returns false unless exactly
/// one operand is a scalar and the other is a constant.
bool splitScalarConstant(const Condition& cond, const ConditionOperand** scalar,
                         const ConditionOperand** constant, bool* scalarIsLhs) {
    if (!cond.lhs() || !cond.rhs()) return false;
    const ConditionOperand& l = *cond.lhs();
    const ConditionOperand& r = *cond.rhs();
    if (l.isScalar() && r.isConstant()) {
        *scalar = &l;
        *constant = &r;
        *scalarIsLhs = true;
        return true;
    }
    if (l.isConstant() && r.isScalar()) {
        *scalar = &r;
        *constant = &l;
        *scalarIsLhs = false;
        return true;
    }
    return false;
}

}  // namespace

bool isRp1EvaluableCondition(const Condition& cond) {
    if (cond.isAlways() || cond.isEpsilonCompare()) return false;

    const ConditionOperand* scalar = nullptr;
    const ConditionOperand* constant = nullptr;
    bool scalarIsLhs = true;
    if (!splitScalarConstant(cond, &scalar, &constant, &scalarIsLhs)) return false;

    const ScalarType type = scalar->type();
    if (!isIntegerScalarType(type)) return false;
    if (scalarBitWidth(type) > 32) return false;  // signal slot is 32-bit

    // Normalise to `scalar OP constant`.
    CompareOp op = cond.op();
    if (!scalarIsLhs) op = flipOperands(op);

    // Ordering comparisons are unsigned on RP1; reject signed scalars.
    if (isOrderingCompare(op) && isSignedIntegerScalarType(type)) return false;

    // Constant must fit in the 32-bit slot.
    const std::uint64_t bits = constant->constantBits();
    if (bits > std::numeric_limits<std::uint32_t>::max()) return false;
    const std::uint32_t value = static_cast<std::uint32_t>(bits);

    // LE/GT are rewritten to LT/GE with value +/- 1; reject if that overflows.
    if (op == CompareOp::LE && value == std::numeric_limits<std::uint32_t>::max()) {
        // `s <= UINT32_MAX` is always true; not a meaningful loop/branch gate.
        return false;
    }
    if (op == CompareOp::GT && value == std::numeric_limits<std::uint32_t>::max()) {
        // `s > UINT32_MAX` is always false.
        return false;
    }
    return true;
}

Rp1Compare mapRp1Condition(const Condition& cond) {
    if (!isRp1EvaluableCondition(cond)) {
        throw std::logic_error("mapRp1Condition: condition is not RP1-evaluable");
    }

    const ConditionOperand* scalar = nullptr;
    const ConditionOperand* constant = nullptr;
    bool scalarIsLhs = true;
    splitScalarConstant(cond, &scalar, &constant, &scalarIsLhs);

    CompareOp op = cond.op();
    if (!scalarIsLhs) op = flipOperands(op);

    Rp1Compare out;
    out.scalarName    = scalar->name();
    out.scalarScopeId = scalar->scopeId();
    out.scalarType    = scalar->type();
    out.value         = static_cast<std::uint32_t>(constant->constantBits());

    switch (op) {
        case CompareOp::EQ: out.op = RP1_COP_EQ; break;
        case CompareOp::NE: out.op = RP1_COP_NE; break;
        case CompareOp::LT: out.op = RP1_COP_LT; break;
        case CompareOp::GE: out.op = RP1_COP_GE; break;
        case CompareOp::LE:  // s <= v  <=>  s < v + 1
            out.op = RP1_COP_LT;
            out.value += 1u;
            break;
        case CompareOp::GT:  // s > v   <=>  s >= v + 1
            out.op = RP1_COP_GE;
            out.value += 1u;
            break;
        default:
            throw std::logic_error("mapRp1Condition: unexpected operator after normalisation");
    }
    return out;
}

rp1_condop_t invertRp1Op(rp1_condop_t op) {
    switch (op) {
        case RP1_COP_EQ:     return RP1_COP_NE;
        case RP1_COP_NE:     return RP1_COP_EQ;
        case RP1_COP_LT:     return RP1_COP_GE;
        case RP1_COP_GE:     return RP1_COP_LT;
        case RP1_COP_AND_NZ: return RP1_COP_AND_Z;
        case RP1_COP_AND_Z:  return RP1_COP_AND_NZ;
    }
    throw std::logic_error("invertRp1Op: unknown rp1_condop_t");
}

std::uint32_t SignalSlotAllocator::alloc() {
    for (std::uint32_t s = 0; s < reservations_.size(); ++s) {
        if (reservations_[s] == 0) {
            reservations_[s] = 1;
            return s;
        }
    }
    throw std::runtime_error("SignalSlotAllocator: RP1 signal array exhausted");
}

std::uint8_t LoopIdAllocator::alloc() {
    if (next_ >= RP1_MAX_LOOPS) {
        throw std::runtime_error("LoopIdAllocator: exceeded RP1_MAX_LOOPS loop ids");
    }
    return static_cast<std::uint8_t>(next_++);
}

namespace {

struct BarrierBucket {
    std::uint8_t id = 0;
    std::vector<std::string> slots;

    std::size_t freeCount() const {
        return static_cast<std::size_t>(std::count(slots.begin(), slots.end(), std::string{}));
    }
};

struct BarrierEvent {
    std::string id;
    std::string domain;
    std::vector<std::string> depends;
    std::map<std::string, std::uint32_t> affinity;
    std::int32_t bucket = -1;
    std::int32_t slot = -1;
    bool isPseudo = false;
    bool isTransition = false;
};

struct BarrierDomain {
    std::string id;
    std::optional<std::string> parent;
    std::vector<std::string> children;
    std::vector<BarrierBucket> localBuckets;
    BarrierBucketRange range;
};

std::vector<std::string> dedupSorted(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

class BarrierLowerer {
   public:
    explicit BarrierLowerer(const BarrierLoweringInput& input)
        : root_(input.rootDomain),
          bitsPerBucket_(input.bitsPerBucket),
          maxBuckets_(input.maxBuckets) {
        if (bitsPerBucket_ == 0 || bitsPerBucket_ > 31) {
            throw std::invalid_argument("BarrierLowerer: bitsPerBucket must be in 1..31");
        }
        if (maxBuckets_ == 0 || maxBuckets_ > RP1_MAX_BUCKETS) {
            throw std::invalid_argument("BarrierLowerer: maxBuckets must be in 1..RP1_MAX_BUCKETS");
        }
        for (const auto& d : input.domains) {
            BarrierDomain domain;
            domain.id = d.id;
            domain.parent = d.parent;
            domain.children = d.children;
            domains_.emplace(domain.id, std::move(domain));
        }
        if (domains_.find(root_) == domains_.end()) {
            throw std::invalid_argument("BarrierLowerer: root domain is missing");
        }
        for (const auto& e : input.events) {
            if (domains_.find(e.domain) == domains_.end()) {
                throw std::invalid_argument(
                    "BarrierLowerer: event '" + e.id + "' references unknown domain '" +
                    e.domain + "'");
            }
            BarrierEvent event;
            event.id = e.id;
            event.domain = e.domain;
            event.depends = e.depends;
            if (!events_.emplace(event.id, std::move(event)).second) {
                throw std::invalid_argument("BarrierLowerer: duplicate event '" + e.id + "'");
            }
        }
    }

    BarrierLoweringResult run() {
        validateDomainEdges();
        insertTransitions();
        allocateDomainTree(root_);
        assignRanges(root_);
        return buildResult();
    }

   private:
    bool isAncestor(const std::string& maybeAncestor, const std::string& domain) const {
        auto cur = domains_.at(domain).parent;
        while (cur) {
            if (*cur == maybeAncestor) return true;
            cur = domains_.at(*cur).parent;
        }
        return false;
    }

    void validateDomainEdges() const {
        for (const auto& [id, event] : events_) {
            (void)id;
            for (const auto& depId : event.depends) {
                const auto depIt = events_.find(depId);
                if (depIt == events_.end()) {
                    throw std::runtime_error(
                        "BarrierLowerer: event '" + event.id + "' depends on unknown event '" +
                        depId + "'");
                }
                const auto& dep = depIt->second;
                if (dep.domain == event.domain) continue;
                if (isAncestor(dep.domain, event.domain)) continue;
                throw std::runtime_error(
                    "BarrierLowerer: dependency escapes reset domain: '" + dep.id +
                    "' -> '" + event.id + "'");
            }
        }
    }

    void insertTransitions() {
        std::vector<std::string> ids;
        ids.reserve(events_.size());
        for (const auto& [id, event] : events_) {
            (void)event;
            ids.push_back(id);
        }
        std::map<std::pair<std::string, std::string>, std::string> cache;
        for (const auto& id : ids) {
            auto& event = events_.at(id);
            std::vector<std::string> rewritten;
            rewritten.reserve(event.depends.size());
            for (const auto& depId : event.depends) {
                const auto& dep = events_.at(depId);
                if (dep.domain == event.domain) {
                    rewritten.push_back(depId);
                    continue;
                }
                rewritten.push_back(getOrCreateTransition(dep.id, event.domain, cache));
            }
            event.depends = dedupSorted(std::move(rewritten));
        }
    }

    std::string getOrCreateTransition(
        const std::string& source,
        const std::string& targetDomain,
        std::map<std::pair<std::string, std::string>, std::string>& cache) {
        const auto key = std::make_pair(source, targetDomain);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;

        BarrierEvent transition;
        transition.id = "__transition_" + std::to_string(nextTransition_++);
        transition.domain = targetDomain;
        transition.depends = {source};
        transition.isTransition = true;
        const std::string id = transition.id;
        events_.emplace(id, std::move(transition));
        syntheticNodes_.push_back(
            BarrierSyntheticNode{BarrierSyntheticNode::Kind::Transition, id, targetDomain,
                                 {source}, id});
        cache[key] = id;
        return id;
    }

    std::vector<std::string> allocatedAncestorEvents(const std::string& domain) const {
        std::set<std::string> ancestors;
        auto cur = domains_.at(domain).parent;
        while (cur) {
            ancestors.insert(*cur);
            cur = domains_.at(*cur).parent;
        }
        std::vector<std::string> out;
        for (const auto& [id, event] : events_) {
            if (event.bucket >= 0 && ancestors.count(event.domain) > 0) out.push_back(id);
        }
        return out;
    }

    std::vector<std::string> domainEvents(const std::string& domain) const {
        std::vector<std::string> out;
        for (const auto& [id, event] : events_) {
            if (event.domain == domain) out.push_back(id);
        }
        return out;
    }

    void allocateDomainTree(const std::string& domain) {
        std::vector<std::string> view = allocatedAncestorEvents(domain);
        auto local = domainEvents(domain);
        view.insert(view.end(), local.begin(), local.end());
        allocateEventsInDomain(domain, view, domains_.at(domain).localBuckets);
        for (const auto& child : domains_.at(domain).children) {
            allocateDomainTree(child);
        }
    }

    void allocateEventsInDomain(const std::string& domain, std::vector<std::string> view,
                                std::vector<BarrierBucket>& buckets) {
        removeUnplacedPseudoEvents(domain, view);
        createPseudoEvents(domain, view);
        recalculateAffinities(domain, view);

        auto remaining = unallocatedInDomain(domain, view);
        if (remaining.empty()) return;

        std::sort(buckets.begin(), buckets.end(), [](const auto& a, const auto& b) {
            return a.freeCount() < b.freeCount();
        });
        for (auto& bucket : buckets) {
            if (bucket.freeCount() >= remaining.size()) {
                putEvents(bucket, remaining);
                return;
            }
        }

        auto& bucket = largestFreeBucketOrNew(buckets);
        remaining = smartSort(std::move(remaining));
        const std::size_t n = std::min(bucket.freeCount(), remaining.size());
        putEvents(bucket, std::vector<std::string>(remaining.begin(), remaining.begin() + n));
        allocateEventsInDomain(domain, std::move(view), buckets);
    }

    std::vector<std::string> unallocatedInDomain(const std::string& domain,
                                                 const std::vector<std::string>& view) const {
        std::vector<std::string> out;
        for (const auto& id : view) {
            const auto& event = events_.at(id);
            if (event.domain == domain && event.bucket < 0) out.push_back(id);
        }
        return out;
    }

    BarrierBucket& largestFreeBucketOrNew(std::vector<BarrierBucket>& buckets) {
        auto best = std::max_element(buckets.begin(), buckets.end(), [](const auto& a, const auto& b) {
            return a.freeCount() < b.freeCount();
        });
        if (best != buckets.end() && best->freeCount() > 0) return *best;
        if (allocatedBucketCount_ >= maxBuckets_) {
            throw std::runtime_error("BarrierLowerer: exhausted RP1 barrier buckets");
        }
        BarrierBucket bucket;
        bucket.id = allocatedBucketCount_++;
        bucket.slots.assign(bitsPerBucket_, "");
        buckets.push_back(std::move(bucket));
        return buckets.back();
    }

    void putEvents(BarrierBucket& bucket, const std::vector<std::string>& ids) {
        for (const auto& id : ids) {
            auto& event = events_.at(id);
            if (event.bucket >= 0) continue;
            auto slot = std::find(bucket.slots.begin(), bucket.slots.end(), "");
            if (slot == bucket.slots.end()) {
                throw std::logic_error("BarrierLowerer: selected bucket is full");
            }
            event.bucket = bucket.id;
            event.slot = static_cast<std::int32_t>(slot - bucket.slots.begin());
            *slot = id;
        }
    }

    bool depsFitOneAwaitBucket(const std::map<std::int32_t, std::vector<std::string>>& byBucket,
                               const std::vector<std::string>& freeDeps) const {
        return byBucket.empty() || (byBucket.size() == 1 && freeDeps.empty());
    }

    void createPseudoEvents(const std::string& domain, std::vector<std::string>& view) {
        std::map<std::vector<std::string>, std::string> collectorByDeps;
        for (const auto& id : view) {
            const auto& event = events_.at(id);
            if (event.domain == domain && event.isPseudo && event.bucket >= 0) {
                collectorByDeps[dedupSorted(event.depends)] = id;
            }
        }
        const auto snapshot = view;
        for (const auto& id : snapshot) {
            auto& event = events_.at(id);
            if (event.domain != domain || event.isPseudo || event.depends.size() <= 1) continue;
            std::map<std::int32_t, std::vector<std::string>> depsByBucket;
            std::vector<std::string> freeDeps;
            for (const auto& depId : event.depends) {
                const auto& dep = events_.at(depId);
                if (dep.bucket >= 0) depsByBucket[dep.bucket].push_back(depId);
                else freeDeps.push_back(depId);
            }
            if (depsFitOneAwaitBucket(depsByBucket, freeDeps)) continue;

            std::vector<std::string> rewritten;
            for (auto& [bucket, deps] : depsByBucket) {
                (void)bucket;
                deps = dedupSorted(std::move(deps));
                auto it = collectorByDeps.find(deps);
                if (it == collectorByDeps.end()) {
                    BarrierEvent pseudo;
                    pseudo.id = "__collector_" + std::to_string(nextCollector_++);
                    pseudo.domain = domain;
                    pseudo.depends = deps;
                    pseudo.isPseudo = true;
                    const std::string pid = pseudo.id;
                    events_.emplace(pid, std::move(pseudo));
                    view.push_back(pid);
                    syntheticNodes_.push_back(
                        BarrierSyntheticNode{BarrierSyntheticNode::Kind::Collector, pid, domain,
                                             deps, pid});
                    it = collectorByDeps.emplace(deps, pid).first;
                }
                rewritten.push_back(it->second);
            }
            rewritten.insert(rewritten.end(), freeDeps.begin(), freeDeps.end());
            event.depends = dedupSorted(std::move(rewritten));
        }
    }

    std::vector<std::string> expandDeps(
        const std::vector<std::string>& deps,
        const std::map<std::string, std::vector<std::string>>& expansions) const {
        std::vector<std::string> out;
        for (const auto& dep : deps) {
            auto it = expansions.find(dep);
            if (it == expansions.end()) {
                out.push_back(dep);
            } else {
                auto expanded = expandDeps(it->second, expansions);
                out.insert(out.end(), expanded.begin(), expanded.end());
            }
        }
        return dedupSorted(std::move(out));
    }

    void removeUnplacedPseudoEvents(const std::string& domain, std::vector<std::string>& view) {
        std::map<std::string, std::vector<std::string>> expansions;
        for (const auto& id : view) {
            const auto& event = events_.at(id);
            if (event.domain == domain && event.isPseudo && event.bucket < 0) {
                expansions[event.id] = event.depends;
            }
        }
        if (expansions.empty()) return;
        for (auto& [id, deps] : expansions) {
            (void)id;
            deps = expandDeps(deps, expansions);
        }
        for (const auto& id : view) {
            auto& event = events_.at(id);
            if (event.domain == domain && event.isPseudo && event.bucket < 0) continue;
            std::vector<std::string> rewritten;
            for (const auto& dep : event.depends) {
                auto it = expansions.find(dep);
                if (it == expansions.end()) rewritten.push_back(dep);
                else rewritten.insert(rewritten.end(), it->second.begin(), it->second.end());
            }
            event.depends = dedupSorted(std::move(rewritten));
        }
        view.erase(std::remove_if(view.begin(), view.end(), [&](const std::string& id) {
            const auto& event = events_.at(id);
            return event.domain == domain && event.isPseudo && event.bucket < 0;
        }), view.end());
        for (const auto& [id, deps] : expansions) {
            (void)deps;
            events_.erase(id);
            syntheticNodes_.erase(
                std::remove_if(syntheticNodes_.begin(), syntheticNodes_.end(),
                               [&](const auto& n) { return n.set == id; }),
                syntheticNodes_.end());
        }
    }

    void recalculateAffinities(const std::string& domain,
                               const std::vector<std::string>& view) {
        for (const auto& id : view) {
            if (events_.at(id).domain == domain) events_.at(id).affinity.clear();
        }
        for (const auto& id : view) {
            const auto& event = events_.at(id);
            if (event.domain != domain) continue;
            for (std::size_t i = 0; i < event.depends.size(); ++i) {
                for (std::size_t j = i + 1; j < event.depends.size(); ++j) {
                    events_.at(event.depends[i]).affinity[event.depends[j]]++;
                    events_.at(event.depends[j]).affinity[event.depends[i]]++;
                }
            }
        }
    }

    std::vector<std::string> smartSort(std::vector<std::string> ids) const {
        if (ids.empty()) return ids;
        auto totalAffinity = [&](const std::string& id) {
            std::uint32_t total = 0;
            for (const auto& [other, weight] : events_.at(id).affinity) {
                (void)other;
                total += weight;
            }
            return total;
        };
        auto best = std::max_element(ids.begin(), ids.end(), [&](const auto& a, const auto& b) {
            return totalAffinity(a) < totalAffinity(b);
        });
        std::swap(ids.front(), *best);
        for (std::size_t i = 1; i < ids.size(); ++i) {
            auto bestIt = ids.begin() + static_cast<std::ptrdiff_t>(i);
            std::uint32_t bestScore = 0;
            for (auto it = bestIt; it != ids.end(); ++it) {
                std::uint32_t score = 0;
                for (std::size_t j = 0; j < i; ++j) {
                    auto aff = events_.at(*it).affinity.find(ids[j]);
                    if (aff != events_.at(*it).affinity.end()) score += aff->second;
                }
                if (score >= bestScore) {
                    bestScore = score;
                    bestIt = it;
                }
            }
            std::swap(ids[i], *bestIt);
        }
        return ids;
    }

    BarrierBucketRange assignRanges(const std::string& domain) {
        auto& d = domains_.at(domain);
        BarrierBucketRange range;
        bool any = false;
        for (const auto& bucket : d.localBuckets) {
            if (!any) {
                range.start = bucket.id;
                range.end = bucket.id;
                range.empty = false;
                any = true;
            } else {
                range.start = std::min(range.start, bucket.id);
                range.end = std::max(range.end, bucket.id);
            }
        }
        for (const auto& child : d.children) {
            auto childRange = assignRanges(child);
            if (childRange.empty) continue;
            if (!any) {
                range = childRange;
                any = true;
            } else {
                range.start = std::min(range.start, childRange.start);
                range.end = std::max(range.end, childRange.end);
            }
        }
        d.range = range;
        return range;
    }

    BarrierLoweringResult buildResult() {
        BarrierLoweringResult result;
        for (const auto& [id, event] : events_) {
            if (event.bucket < 0 || event.slot < 0) {
                throw std::logic_error("BarrierLowerer: event '" + id + "' was not allocated");
            }
            result.events[id] = BarrierPhysicalEvent{
                static_cast<std::uint8_t>(event.bucket),
                static_cast<std::uint32_t>(1u << event.slot)};
        }
        for (const auto& [id, domain] : domains_) {
            result.domainRanges[id] = domain.range;
        }
        result.syntheticNodes = syntheticNodes_;
        return result;
    }

    std::string root_;
    std::map<std::string, BarrierDomain> domains_;
    std::map<std::string, BarrierEvent> events_;
    std::vector<BarrierSyntheticNode> syntheticNodes_;
    std::uint8_t bitsPerBucket_ = 31;
    std::uint8_t maxBuckets_ = RP1_MAX_BUCKETS;
    std::uint8_t allocatedBucketCount_ = 0;
    std::uint32_t nextCollector_ = 0;
    std::uint32_t nextTransition_ = 0;
};

}  // namespace

BarrierLoweringResult lowerBarrierEvents(const BarrierLoweringInput& input) {
    BarrierLowerer lowerer(input);
    auto result = lowerer.run();
    return result;
}

}  // namespace vrt::graph::fpga
