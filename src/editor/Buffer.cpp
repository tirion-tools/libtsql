#include "Buffer.h"

#include <algorithm>

#include "tsql/ast/generated/token_types.hpp"

namespace tsql::editor::detail {

Buffer::Buffer(const Grammar& grammar) : grammar_(&grammar) { SetText({}); }

TokenSourceView Buffer::View() const {
    TokenSourceView v;
    v.text = text_;
    v.tokens = &tokens_;
    v.dualQuoted = &dual_;
    v.invalidUtf8 = invalidUtf8_;
    return v;
}

namespace {
bool HasInvalidUtf8(std::string_view s) {
    std::string unused;
    return SanitizeUtf8(s, unused);
}
}  // namespace

void Buffer::SetText(std::string_view sql) {
    text_.assign(sql.data(), sql.size());
    invalidUtf8_ = HasInvalidUtf8(text_);
    tokens_.clear();
    dual_.clear();
    lookEnd_.clear();
    goAfter_.clear();
    grammar_->LexFrom(text_, 0, true, [&](const LexedToken& t) {
        tokens_.push_back(t.token);
        dual_.push_back(t.dualQuoted);
        lookEnd_.push_back(std::max(t.lookEnd, lookEnd_.empty() ? 0u : lookEnd_.back()));
        goAfter_.push_back(t.goAfter);
        return true;
    });
    visibleValid_ = false;
    gos_.clear();
    for (size_t i = 0; i < tokens_.size(); ++i)
        if (tokens_[i].type == static_cast<uint32_t>(ast::TSqlTokenType::Go)) gos_.push_back(static_cast<uint32_t>(i));
    roles_.assign(tokens_.size(), TokenRole{});
    runs_.clear();
}

void Buffer::Edit(size_t start, size_t length, std::string_view replacement) {
    start = std::min(start, text_.size());
    length = std::min(length, text_.size() - start);
    if (length == 0 && replacement.empty()) return;

    // ---- tokens: lex again from the end of the last token whose lexing did not look at the edit
    const size_t first = static_cast<size_t>(std::upper_bound(lookEnd_.begin(), lookEnd_.end(), start) - lookEnd_.begin());
    const size_t restart = first > 0 ? tokens_[first - 1].end : 0;
    const bool go = first > 0 ? goAfter_[first - 1] != 0 : true;
    const auto delta = static_cast<std::ptrdiff_t>(replacement.size()) - static_cast<std::ptrdiff_t>(length);
    const size_t newEditEnd = start + replacement.size();
    text_.replace(start, length, replacement.data(), replacement.size());
    invalidUtf8_ = HasInvalidUtf8(text_);

    std::vector<LexedToken> fresh;
    size_t resync = SIZE_MAX;   // the old token the new tokens resynchronise with
    grammar_->LexFrom(text_, restart, go, [&](const LexedToken& t) {
        fresh.push_back(t);
        if (t.token.end < newEditEnd) return true;
        // an old token that ended at the same (shifted) place, with the lexer in the same state
        // after it: the rest of the text is unchanged, so the old tokens after it hold
        const auto oldEnd = static_cast<uint32_t>(static_cast<std::ptrdiff_t>(t.token.end) - delta);
        auto it = std::lower_bound(tokens_.begin() + static_cast<std::ptrdiff_t>(first), tokens_.end(), oldEnd,
                                   [](const LexToken& a, uint32_t e) { return a.end < e; });
        if (it == tokens_.end() || it->end != oldEnd) return true;
        const size_t k = static_cast<size_t>(it - tokens_.begin());
        if ((goAfter_[k] != 0) != t.goAfter) return true;
        resync = k;
        return false;
    });
    const size_t oldEnd = resync == SIZE_MAX ? tokens_.size() : resync + 1;   // old tokens [first, oldEnd) are replaced
    const size_t count = fresh.size();
    const size_t newEnd = first + count;
    const auto shift = static_cast<std::ptrdiff_t>(newEnd) - static_cast<std::ptrdiff_t>(oldEnd);

    auto splice = [&](auto& v, const auto& value) {
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(first), v.begin() + static_cast<std::ptrdiff_t>(oldEnd));
        v.insert(v.begin() + static_cast<std::ptrdiff_t>(first), count, value);
    };
    splice(tokens_, LexToken{});
    splice(dual_, uint8_t{0});
    splice(lookEnd_, uint32_t{0});
    splice(goAfter_, uint8_t{0});
    splice(roles_, TokenRole{});
    for (size_t k = 0; k < count; ++k) {
        tokens_[first + k] = fresh[k].token;
        dual_[first + k] = fresh[k].dualQuoted;
        goAfter_[first + k] = fresh[k].goAfter;
        lookEnd_[first + k] = std::max(fresh[k].lookEnd, first + k > 0 ? lookEnd_[first + k - 1] : 0u);
    }
    for (size_t k = newEnd; k < tokens_.size(); ++k) {
        LexToken& t = tokens_[k];
        t.start = static_cast<uint32_t>(t.start + delta);
        t.end = static_cast<uint32_t>(t.end + delta);
        lookEnd_[k] = std::max(static_cast<uint32_t>(lookEnd_[k] + delta), k > 0 ? lookEnd_[k - 1] : 0u);
    }
    // ---- the parser-visible tokens: splice the relexed ones in, shift the rest (no full rebuild)
    if (visibleValid_) {
        const auto vFirst = std::lower_bound(visibleToToken_.begin(), visibleToToken_.end(), static_cast<uint32_t>(first)) -
                            visibleToToken_.begin();
        const auto vOld = std::lower_bound(visibleToToken_.begin() + vFirst, visibleToToken_.end(),
                                           static_cast<uint32_t>(oldEnd)) -
                          visibleToToken_.begin();
        std::vector<uint32_t> added;
        for (size_t k = first; k < newEnd; ++k)
            if (!IsHiddenType(tokens_[k].type)) added.push_back(static_cast<uint32_t>(k));
        visibleToToken_.erase(visibleToToken_.begin() + vFirst, visibleToToken_.begin() + vOld);
        visibleToToken_.insert(visibleToToken_.begin() + vFirst, added.begin(), added.end());
        visible_.erase(visible_.begin() + vFirst, visible_.begin() + vOld);
        visible_.insert(visible_.begin() + vFirst, added.size(), LexToken{});
        const size_t from = static_cast<size_t>(vFirst) + added.size();
        for (size_t j = from; j < visibleToToken_.size(); ++j)
            visibleToToken_[j] = static_cast<uint32_t>(static_cast<std::ptrdiff_t>(visibleToToken_[j]) + shift);
        for (size_t j = static_cast<size_t>(vFirst); j < visible_.size(); ++j) visible_[j] = tokens_[visibleToToken_[j]];
    }

    // ---- the parse: runs after the edit's GO stand; a run the edit is in keeps the resume points
    // whose look ended before the first changed token; runs whose GO was lexed again are dropped
    std::vector<Run> runs;
    runs.reserve(runs_.size());
    for (Run& r : runs_) {
        if (r.start > first) {
            if (r.start - 1 < oldEnd) continue;
            Shift(r, shift);
        } else {
            Truncate(r, first, oldEnd, newEnd);
            // a parse that ended before its region's GO (an escaped error) left the region's roles
            // after it empty: when the edit took that GO, the region grows by tokens that still
            // hold the roles of another parse, so the parse is run again to its end
            auto go = std::lower_bound(gos_.begin(), gos_.end(), r.start);
            if (r.done && go != gos_.end() && *go >= first && *go < oldEnd) r.done = false;
        }
        runs.push_back(std::move(r));
    }
    runs_ = std::move(runs);

    // ---- GO tokens: the old ones lexed again are replaced by the new ones, the later ones shift
    {
        auto lo = std::lower_bound(gos_.begin(), gos_.end(), static_cast<uint32_t>(first));
        auto hi = std::lower_bound(lo, gos_.end(), static_cast<uint32_t>(oldEnd));
        for (auto it = hi; it != gos_.end(); ++it) *it = static_cast<uint32_t>(*it + shift);
        std::vector<uint32_t> added;
        for (size_t k = first; k < newEnd; ++k)
            if (tokens_[k].type == static_cast<uint32_t>(ast::TSqlTokenType::Go)) added.push_back(static_cast<uint32_t>(k));
        gos_.insert(gos_.erase(lo, hi), added.begin(), added.end());
    }
}

const std::vector<LexToken>& Buffer::Visible() {
    if (!visibleValid_) {
        visible_.clear();
        visibleToToken_.clear();
        for (size_t i = 0; i < tokens_.size(); ++i) {
            if (IsHiddenType(tokens_[i].type)) continue;
            visible_.push_back(tokens_[i]);
            visibleToToken_.push_back(static_cast<uint32_t>(i));
        }
        visibleValid_ = true;
    }
    return visible_;
}

const std::vector<uint32_t>& Buffer::VisibleToToken() {
    Visible();
    return visibleToToken_;
}

size_t Buffer::TokenAt(size_t offset) const {
    return static_cast<size_t>(std::lower_bound(tokens_.begin(), tokens_.end(), offset,
                                                [](const LexToken& t, size_t o) { return t.start < o; }) -
                               tokens_.begin());
}

// ============================================================================================ parse

size_t Buffer::RegionStart(size_t index) const {
    auto it = std::lower_bound(gos_.begin(), gos_.end(), static_cast<uint32_t>(std::min<size_t>(index, UINT32_MAX)));
    return it == gos_.begin() ? 0 : *(it - 1) + 1;
}

size_t Buffer::RegionEnd(size_t start) const {
    auto it = std::lower_bound(gos_.begin(), gos_.end(), static_cast<uint32_t>(start));
    return it == gos_.end() ? tokens_.size() : *it + 1;
}

Buffer::Point Buffer::Root(size_t start) const {
    Point root{ResumePoint{}, 0};   // the script start
    if (start == 0) return root;
    // after GO the script's parse calls batch() at the next parser-visible token, with
    // QUOTED_IDENTIFIER reset to its initial setting (ON); where that is depends on the tokens up to it
    size_t k = start;
    while (k < tokens_.size() && IsHiddenType(tokens_[k].type)) ++k;
    root.at.kind = ResumePoint::Kind::BatchStart;
    root.at.token = static_cast<uint32_t>(k);
    root.lookEnd = static_cast<uint32_t>(k + 1);
    return root;
}

const Buffer::Run* Buffer::FindRun(size_t start) const {
    auto it = std::lower_bound(runs_.begin(), runs_.end(), start, [](const Run& r, size_t s) { return r.start < s; });
    return it != runs_.end() && it->start == start ? &*it : nullptr;
}

Buffer::Run& Buffer::RunAt(size_t start) {
    auto it = std::lower_bound(runs_.begin(), runs_.end(), start, [](const Run& r, size_t s) { return r.start < s; });
    if (it != runs_.end() && it->start == start) return *it;
    Run r;
    r.start = static_cast<uint32_t>(start);
    r.points.push_back(Root(start));
    return *runs_.insert(it, std::move(r));
}

void Buffer::EnsureParsed(size_t first, size_t end) {
    size_t i = first;
    do {
        const size_t start = RegionStart(i), regionEnd = RegionEnd(start);
        Extend(RunAt(start), std::min(end, regionEnd));
        i = regionEnd;
    } while (i < end && i < tokens_.size());
}

void Buffer::Extend(Run& run, size_t end) {
    while (!run.done && run.points.back().at.token < end) {
        current_ = &run;
        target_ = end;
        pendingKeyword_.clear();
        const size_t before = run.points.size();
        grammar_->ParseFrom(View(), run.points.back().at, *this);
        if (run.points.size() == before && !run.done) run.done = true;   // no progress: cannot happen
    }
    current_ = nullptr;
}

void Buffer::Finish(Run& run, uint32_t look) {
    run.done = true;
    run.doneLook = std::max(look, run.points.back().lookEnd);
    run.stale.clear();
    run.staleHead = 0;
    run.staleDone = false;
}

void Buffer::Shift(Run& run, std::ptrdiff_t shift) {
    auto by = [shift](uint32_t& v) { v = static_cast<uint32_t>(static_cast<std::ptrdiff_t>(v) + shift); };
    by(run.start);
    for (Point& p : run.points) {
        by(p.at.token);
        by(p.lookEnd);
    }
    for (Point& p : run.stale) {
        by(p.at.token);
        by(p.lookEnd);
    }
    by(run.doneLook);
    by(run.staleDoneLook);
    by(run.dirtyEnd);
}

void Buffer::Truncate(Run& r, size_t first, size_t oldEnd, size_t newEnd) {
    const auto shift = static_cast<std::ptrdiff_t>(newEnd) - static_cast<std::ptrdiff_t>(oldEnd);
    uint32_t look = r.points.back().lookEnd;   // how far anything the run holds looked
    if (r.done) look = std::max(look, r.doneLook);
    if (r.staleHead < r.stale.size()) look = std::max(look, r.stale.back().lookEnd);
    if (r.staleDone) look = std::max(look, r.staleDoneLook);
    if (look <= first) return;   // all of it before the edit

    size_t keep = 1;
    while (keep < r.points.size() && r.points[keep].lookEnd <= first) ++keep;
    // the rest of the earlier parse past the change may be taken over once the parse gets there
    // again (an older earlier parse's points, not yet reached since the edit before, come first:
    // the tokens between them and this edit's are unknown to the current parse)
    const bool olderStale = r.staleHead < r.stale.size();
    std::vector<Point> stale;
    auto takeOver = [&](const Point& p) {
        if (p.at.token < oldEnd) return;
        Point q = p;
        q.at.token = static_cast<uint32_t>(q.at.token + shift);
        q.lookEnd = static_cast<uint32_t>(q.lookEnd + shift);
        stale.push_back(q);
    };
    size_t dirty = newEnd;
    bool staleDone;
    uint32_t staleDoneLook;
    if (olderStale) {
        for (size_t k = r.staleHead; k < r.stale.size(); ++k) takeOver(r.stale[k]);
        if (r.dirtyEnd >= oldEnd) dirty = std::max(dirty, static_cast<size_t>(static_cast<std::ptrdiff_t>(r.dirtyEnd) + shift));
        staleDone = r.staleDone && r.staleDoneLook >= oldEnd;
        staleDoneLook = r.staleDoneLook;
    } else {
        for (size_t k = keep; k < r.points.size(); ++k) takeOver(r.points[k]);
        staleDone = r.done && r.doneLook >= oldEnd;
        staleDoneLook = r.doneLook;
    }
    stale.erase(std::remove_if(stale.begin(), stale.end(), [&](const Point& p) { return p.at.token < dirty; }), stale.end());
    r.staleDone = staleDone && !stale.empty();
    r.staleDoneLook = r.staleDone ? static_cast<uint32_t>(staleDoneLook + shift) : 0;
    r.stale = std::move(stale);
    r.staleHead = 0;
    r.dirtyEnd = static_cast<uint32_t>(dirty);
    r.points.resize(keep);
    // the token the root stands at may be one the edit changed (the region's first visible token)
    if (keep == 1) r.points[0] = Root(r.start);
    if (r.done && r.doneLook > first) r.done = false;
}

std::vector<Buffer::Point>::const_iterator Buffer::PointsEnd(const Run& run, size_t index, size_t lookLimit) {
    // lookEnd does not decrease along the points: the usable points are a prefix of them
    auto byToken = std::upper_bound(run.points.begin(), run.points.end(), index,
                                    [](size_t i, const Point& p) { return i < p.at.token; });
    auto byLook = std::upper_bound(run.points.begin(), run.points.end(), lookLimit,
                                   [](size_t l, const Point& p) { return l < p.lookEnd; });
    return std::min(byToken, byLook);
}

ResumePoint Buffer::ResumeAtOrBefore(size_t index, size_t lookLimit) const {
    const size_t start = RegionStart(index);
    const Run* run = FindRun(start);
    if (run == nullptr) return Root(start).at;
    auto it = PointsEnd(*run, index, lookLimit);
    return it == run->points.begin() ? run->points.front().at : (it - 1)->at;
}

ResumePoint Buffer::BatchResumeAtOrBefore(size_t index) const { return Root(RegionStart(index)).at; }

void Buffer::OnRole(size_t index, const TokenRole& role) {
    if (index >= roles_.size()) return;
    TokenRole r = role;
    r.predicateKeyword = false;
    size_t keepPending = 0;
    for (uint32_t p : pendingKeyword_) {
        if (p == index) r.predicateKeyword = true;
        else if (p > index) pendingKeyword_[keepPending++] = p;
    }
    pendingKeyword_.resize(keepPending);
    roles_[index] = r;
}

void Buffer::OnPredicateKeyword(size_t index) { pendingKeyword_.push_back(static_cast<uint32_t>(index)); }

bool Buffer::OnCheckpoint(const ResumePoint& at, size_t lookEnd) {
    Run& r = *current_;
    if (at == r.points.back().at) return true;   // the point the parse resumed from
    const auto look = static_cast<uint32_t>(std::max<size_t>(lookEnd, r.points.back().lookEnd));
    if (at.kind == ResumePoint::Kind::BatchStart) {
        // past the region's GO: the next region's run starts here
        Finish(r, look);
        return false;
    }
    r.points.push_back(Point{at, look});
    while (r.staleHead < r.stale.size() && r.stale[r.staleHead].at.token < at.token) ++r.staleHead;
    for (size_t k = r.staleHead; k < r.stale.size() && r.stale[k].at.token == at.token; ++k) {
        if (!(r.stale[k].at == at)) continue;
        // the earlier parse passed here in the same state and the tokens from here on are the
        // ones it saw: the rest of it holds
        for (size_t m = k + 1; m < r.stale.size(); ++m)
            r.points.push_back(Point{r.stale[m].at, std::max(r.stale[m].lookEnd, r.points.back().lookEnd)});
        const bool done = r.staleDone;
        const uint32_t doneLook = r.staleDoneLook;
        r.stale.clear();
        r.staleHead = 0;
        r.staleDone = false;
        if (done) Finish(r, doneLook);
        return false;
    }
    return at.token < target_;
}

void Buffer::OnEnd(size_t index, size_t lookEnd) {
    // the parse ended (the end of the text, or an error the script rule did not recover from):
    // the region's tokens from here on stay untaken
    Run& r = *current_;
    const size_t end = std::min(RegionEnd(r.start), roles_.size());
    for (size_t k = index; k < end; ++k) roles_[k] = TokenRole{};
    Finish(r, static_cast<uint32_t>(std::min<size_t>(lookEnd, UINT32_MAX)));
}

}  // namespace tsql::editor::detail
