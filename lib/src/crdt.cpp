#include "cloud_data_core/crdt.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace cloud_data_core {

std::string OrSetTag::toString() const {
    return peerId + "#" + std::to_string(counter);
}

OrSetTag OrSetTag::fromString(const std::string& s) {
    OrSetTag tag;
    auto pos = s.rfind('#');
    if (pos == std::string::npos) return tag;
    tag.peerId = s.substr(0, pos);
    tag.counter = std::stoull(s.substr(pos + 1));
    return tag;
}

void LwwRegister::apply(const std::string& valueJson, const HlcTimestamp& hlc, const std::string& peerId) {
    if (!hasValue_ || hlcWinsOver(hlc, peerId, hlc_, peerId_)) {
        valueJson_ = valueJson;
        hlc_ = hlc;
        peerId_ = peerId;
        hasValue_ = true;
    }
}

void OrSet::applyAdd(const OrSetTag& tag, const std::string& valueJson) {
    if (removes_.count(tag)) return; // remove already observed for this exact tag
    adds_[tag] = valueJson;
}

void OrSet::applyRemove(const OrSetTag& tag) {
    removes_.insert(tag);
    adds_.erase(tag);
}

std::vector<std::string> OrSet::values() const {
    std::vector<std::string> out;
    out.reserve(adds_.size());
    for (const auto& [tag, valueJson] : adds_) {
        out.push_back(valueJson);
    }
    return out;
}

std::vector<OrSetTag> OrSet::currentTags() const {
    std::vector<OrSetTag> out;
    out.reserve(adds_.size());
    for (const auto& [tag, valueJson] : adds_) {
        (void)valueJson;
        out.push_back(tag);
    }
    return out;
}

void PnCounter::applyDelta(const std::string& peerId, int64_t delta) {
    if (delta >= 0) {
        perPeerIncrements_[peerId] += delta;
    } else {
        perPeerDecrements_[peerId] += -delta;
    }
}

int64_t PnCounter::value() const {
    int64_t total = 0;
    for (const auto& [peer, v] : perPeerIncrements_) { (void)peer; total += v; }
    for (const auto& [peer, v] : perPeerDecrements_) { (void)peer; total -= v; }
    return total;
}

void CrdtDocument::declareField(const std::string& field, CrdtType type) {
    schema_[field] = type;
    switch (type) {
        case CrdtType::LwwRegister: lww_.try_emplace(field); break;
        case CrdtType::OrSet: orSets_.try_emplace(field); break;
        case CrdtType::PnCounter: pnCounters_.try_emplace(field); break;
    }
}

bool CrdtDocument::hasAppliedOp(const std::string& opId) const {
    return appliedOpIds_.count(opId) != 0;
}

void CrdtDocument::applyOp(const CrdtOp& op) {
    if (op.opId.empty() || appliedOpIds_.count(op.opId)) return;
    appliedOpIds_.insert(op.opId);

    if (schema_.find(op.field) == schema_.end()) {
        declareField(op.field, op.type);
    }

    switch (op.type) {
        case CrdtType::LwwRegister:
            lww_[op.field].apply(op.valueJson, op.hlc, op.peerId);
            break;
        case CrdtType::OrSet:
            if (op.isRemove) {
                orSets_[op.field].applyRemove(op.tag);
            } else {
                orSets_[op.field].applyAdd(op.tag, op.valueJson);
            }
            break;
        case CrdtType::PnCounter:
            pnCounters_[op.field].applyDelta(op.peerId, op.delta);
            break;
    }
}

std::vector<OrSetTag> CrdtDocument::currentOrSetTags(const std::string& field) const {
    auto it = orSets_.find(field);
    return it != orSets_.end() ? it->second.currentTags() : std::vector<OrSetTag>{};
}

bool CrdtDocument::isDeleted() const {
    auto it = lww_.find(kDeletedField);
    return it != lww_.end() && it->second.hasValue() && it->second.valueJson() == "true";
}

std::string CrdtDocument::materializeJson() const {
    json obj = json::object();
    for (const auto& [field, type] : schema_) {
        switch (type) {
            case CrdtType::LwwRegister: {
                auto it = lww_.find(field);
                if (it != lww_.end() && it->second.hasValue()) {
                    obj[field] = json::parse(it->second.valueJson(), nullptr, false);
                }
                break;
            }
            case CrdtType::OrSet: {
                auto it = orSets_.find(field);
                json arr = json::array();
                if (it != orSets_.end()) {
                    for (const auto& v : it->second.values()) {
                        arr.push_back(json::parse(v, nullptr, false));
                    }
                }
                obj[field] = arr;
                break;
            }
            case CrdtType::PnCounter: {
                auto it = pnCounters_.find(field);
                obj[field] = (it != pnCounters_.end()) ? it->second.value() : 0;
                break;
            }
        }
    }
    obj[kDeletedField] = isDeleted();
    return obj.dump();
}

} // namespace cloud_data_core
