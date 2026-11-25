#include "disk_model.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace npk::pipeline {

namespace {

constexpr double kStallFactor = 3;     // a window this many typical residuals too slow is a stall: weighed down
constexpr double kScaleWeight = 0.02;  // how fast the typical residual follows

// Weighted median of (value, weight) pairs.
double weighted_median(std::vector<std::pair<double, double>> items)
{
    std::sort(items.begin(), items.end());
    double total = 0;
    for (const auto& item : items) total += item.second;
    double seen = 0;
    for (const auto& [value, weight] : items) {
        seen += weight;
        if (seen >= total / 2) return value;
    }
    return items.back().first;
}

} // namespace

void DiskModel::observe_mix(const Window& window)
{
    if (window.seconds <= 0) return;
    const Window rate{1, window.files / window.seconds, window.bytes_written / window.seconds,
                      window.bytes_read / window.seconds};
    if (!have_mix_) {
        mix_ = rate;
        have_mix_ = true;
        return;
    }
    const double alpha = std::min(window.seconds / kMixSeconds, 1.0);
    mix_.files += alpha * (rate.files - mix_.files);
    mix_.bytes_written += alpha * (rate.bytes_written - mix_.bytes_written);
    mix_.bytes_read += alpha * (rate.bytes_read - mix_.bytes_read);
}

void DiskModel::observe(unsigned writers, const Window& window, std::int64_t at_ns)
{
    writers = std::clamp(writers, 1u, kMaxWriters);
    if (window.seconds <= 0) return;
    PerCount& count = counts_[writers];
    const double f = window.files, r = window.bytes_written, q = window.bytes_read, y = window.seconds;

    // Stalls make a window far too slow; they would inflate every cost.
    double weight = 1;
    if (count.ff > 0) {
        const double residual = y - (a_[writers] * f + b_ * r + c_ * q);
        if (scale_ > 0 && residual > kStallFactor * scale_) weight = kStallFactor * scale_ / residual;
        scale_ += kScaleWeight * (std::abs(residual) * weight - scale_);
    }

    const double keep = std::exp2(-1 / kHalfLifeWindows);
    for (PerCount& c : counts_) {
        c.ff *= keep;
        c.fr *= keep;
        c.fq *= keep;
        c.fy *= keep;
    }
    rr_ *= keep, rq_ *= keep, qq_ *= keep, ry_ *= keep, qy_ *= keep;

    count.ff += weight * f * f;
    count.fr += weight * f * r;
    count.fq += weight * f * q;
    count.fy += weight * f * y;
    rr_ += weight * r * r;
    rq_ += weight * r * q;
    qq_ += weight * q * q;
    ry_ += weight * r * y;
    qy_ += weight * q * y;
    solve();

    // a[W] estimates come from chunks of consecutive windows at one count.
    if (pending_writers_ != writers) break_chunk();
    pending_writers_ = writers;
    pending_.seconds += y;
    pending_.files += f;
    pending_.bytes_written += r;
    pending_.bytes_read += q;
    if (++pending_windows_ == kChunkWindows) {
        if (pending_.files > 0) count.chunks.push_back({pending_, at_ns});
        while (!count.chunks.empty() && (count.chunks.size() > 64 || at_ns - count.chunks.front().at_ns > kChunkMemoryNs)) {
            count.chunks.pop_front();
        }
        pending_ = {};
        pending_windows_ = 0;
    }
}

// Least squares with one a per writer count and shared b, c: the a's are
// eliminated, leaving a 2x2 system for b and c.
void DiskModel::solve()
{
    double mbb = rr_, mbc = rq_, mcc = qq_, vb = ry_, vc = qy_;
    for (const PerCount& c : counts_) {
        if (c.ff <= 0) continue;
        mbb -= c.fr * c.fr / c.ff;
        mbc -= c.fr * c.fq / c.ff;
        mcc -= c.fq * c.fq / c.ff;
        vb -= c.fr * c.fy / c.ff;
        vc -= c.fq * c.fy / c.ff;
    }
    double b = -1, c = -1;
    const double det = mbb * mcc - mbc * mbc;
    if (mbb > 0 && mcc > 0 && det > 1e-9 * mbb * mcc) {
        b = (vb * mcc - vc * mbc) / det;
        c = (vc * mbb - vb * mbc) / det;
    }
    if (!(b >= 0 && c >= 0)) {   // reading and writing can't be told apart (or no reading): bytes written only
        c = 0;
        b = mbb > 0 ? std::max(vb / mbb, 0.0) : 0;
    }
    b_ = b;
    c_ = c;
    for (std::size_t w = 0; w < counts_.size(); ++w) {
        const PerCount& count = counts_[w];
        a_[w] = count.ff > 0 ? (count.fy - b_ * count.fr - c_ * count.fq) / count.ff : 0;
    }
}

std::optional<DiskModel::Estimate> DiskModel::estimate(unsigned writers, std::int64_t now_ns) const
{
    writers = std::clamp(writers, 1u, kMaxWriters);
    std::vector<std::pair<double, double>> items;
    for (const Chunk& chunk : counts_[writers].chunks) {
        if (now_ns - chunk.at_ns > kChunkMemoryNs) continue;
        const Window& s = chunk.sum;
        items.emplace_back((s.seconds - b_ * s.bytes_written - c_ * s.bytes_read) / s.files, s.files);
    }
    if (items.empty()) return std::nullopt;

    Estimate estimate;
    estimate.chunks = items.size();
    estimate.per_file = weighted_median(items);
    if (items.size() < 2) {
        estimate.uncertainty = std::abs(estimate.per_file);
        return estimate;
    }
    double total = 0, squares = 0;
    std::vector<std::pair<double, double>> deviations;
    for (const auto& [value, weight] : items) {
        deviations.emplace_back(std::abs(value - estimate.per_file), weight);
        total += weight;
        squares += weight * weight;
    }
    const double spread = 1.4826 * weighted_median(std::move(deviations));   // MAD as a standard deviation
    const double effective = total * total / squares;
    estimate.uncertainty = spread / std::sqrt(effective);
    return estimate;
}

bool DiskModel::known(unsigned writers, std::int64_t now_ns) const
{
    const auto e = estimate(writers, now_ns);
    return e && e->chunks >= kKnownChunks;
}

std::optional<DiskModel::Prediction> DiskModel::predict(unsigned writers, std::int64_t now_ns) const
{
    const auto e = estimate(writers, now_ns);
    if (!e || !have_mix_) return std::nullopt;
    Prediction p;
    p.seconds = e->per_file * mix_.files + b_ * mix_.bytes_written + c_ * mix_.bytes_read;
    p.uncertainty = e->uncertainty * mix_.files;
    return p;
}

} // namespace npk::pipeline
