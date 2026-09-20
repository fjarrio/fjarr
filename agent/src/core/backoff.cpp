#include "backoff.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace fjarr {

Backoff::Backoff(BackoffPolicy policy, std::function<double()> random) : policy_(policy), random_(std::move(random)) {
    if (!random_) {
        random_ = [] {
            static thread_local std::mt19937 gen{std::random_device{}()};
            return std::uniform_real_distribution<double>(0.0, 1.0)(gen);
        };
    }
}

std::chrono::milliseconds Backoff::next() {
    const double base = static_cast<double>(policy_.initial.count()) * std::pow(policy_.factor, attempts_);
    const double capped = std::min(base, static_cast<double>(policy_.max.count()));
    const double jitter = 1.0 + (random_() * 2.0 - 1.0) * policy_.jitter;
    attempts_++;
    return std::chrono::milliseconds(static_cast<long>(std::max(0.0, capped * jitter)));
}

void Backoff::mark_connected(std::chrono::steady_clock::time_point now) {
    connected_ = true;
    connected_at_ = now;
}

void Backoff::mark_disconnected(std::chrono::steady_clock::time_point now) {
    if (connected_ && now - connected_at_ >= policy_.stable_reset) attempts_ = 0;
    connected_ = false;
}

void Backoff::reset() {
    attempts_ = 0;
    connected_ = false;
}

} // namespace fjarr
