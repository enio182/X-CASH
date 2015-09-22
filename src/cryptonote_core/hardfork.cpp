// Copyright (c) 2015, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <algorithm>
#include <cstdio>

#include "cryptonote_core/cryptonote_basic.h"
#include "blockchain_db/blockchain_db.h"
#include "hardfork.h"

using namespace cryptonote;

HardFork::HardFork(cryptonote::BlockchainDB &db, uint8_t original_version, time_t forked_time, time_t update_time, uint64_t window_size, int threshold_percent):
  db(db),
  original_version(original_version),
  forked_time(forked_time),
  update_time(update_time),
  window_size(window_size),
  threshold_percent(threshold_percent)
{
}

bool HardFork::add(uint8_t version, uint64_t height, time_t time)
{
  CRITICAL_REGION_LOCAL(lock);

  // add in order
  if (version == 0)
    return false;
  if (!heights.empty()) {
    if (version <= heights.back().version)
      return false;
    if (height <= heights.back().height)
      return false;
    if (time <= heights.back().time)
      return false;
  }
  heights.push_back({version: version, height: height, time: time});
  return true;
}

uint8_t HardFork::get_effective_version(const cryptonote::block &block) const
{
  uint8_t version = block.major_version;
  if (!heights.empty()) {
    uint8_t max_version = heights.back().version;
    if (version > max_version)
      version = max_version;
  }
  return version;
}

bool HardFork::do_check(const cryptonote::block &block) const
{
  return block.major_version >= heights[current_fork_index].version;
}

bool HardFork::check(const cryptonote::block &block) const
{
  CRITICAL_REGION_LOCAL(lock);
  return do_check(block);
}

bool HardFork::add(const cryptonote::block &block, uint64_t height)
{
  CRITICAL_REGION_LOCAL(lock);

  if (!do_check(block))
    return false;

  const uint8_t version = get_effective_version(block);

  while (versions.size() >= window_size) {
    const uint8_t old_version = versions.front();
    last_versions[old_version]--;
    assert(last_versions[old_version] >= 0);
    versions.pop_front();
  }

  last_versions[version]++;
  versions.push_back(version);

  uint8_t voted = get_voted_fork_index(height);
  if (voted > current_fork_index) {
    for (int v = heights[current_fork_index].version + 1; v <= heights[voted].version; ++v) {
      db.set_hard_fork_starting_height(v, height);
    }
    current_fork_index = voted;
  }

  db.set_hard_fork_version(height, heights[current_fork_index].version);

  return true;
}

void HardFork::init()
{
  CRITICAL_REGION_LOCAL(lock);
  versions.clear();
  for (size_t n = 0; n < 256; ++n)
    last_versions[n] = 0;
  current_fork_index = 0;
  vote_threshold = (uint32_t)ceilf(window_size * threshold_percent / 100.0f);

  // restore state from DB
  uint64_t height = db.height();
  if (height > window_size)
    height -= window_size;
  else
    height = 1;

  bool populate = db.get_hard_fork_starting_height(original_version) == std::numeric_limits<uint64_t>::max();
  if (populate) {
    LOG_PRINT_L0("The DB has no hard fork info, reparsing from start");
    height = 1;
  }
  LOG_PRINT_L1("reorganizing from " << height);
  reorganize_from_chain_height(height);
  if (populate) {
    // reorg will not touch the genesis block, use this as a flag for populating done
    db.set_hard_fork_version(0, original_version);
    db.set_hard_fork_starting_height(original_version, 0);
  }
  LOG_PRINT_L1("reorganization done");
}

bool HardFork::reorganize_from_block_height(uint64_t height)
{
  CRITICAL_REGION_LOCAL(lock);
  if (height >= db.height())
    return false;

  versions.clear();

  for (size_t n = 0; n < 256; ++n)
    last_versions[n] = 0;
  const uint64_t rescan_height = height >= (window_size - 1) ? height - (window_size - 1) : 0;
  const uint8_t start_version = height == 0 ? original_version : db.get_hard_fork_version(height);
  while (heights[current_fork_index].version > start_version) {
    db.set_hard_fork_starting_height(heights[current_fork_index].version, std::numeric_limits<uint64_t>::max());
    --current_fork_index;
  }
  for (uint64_t h = rescan_height; h <= height; ++h) {
    cryptonote::block b = db.get_block_from_height(h);
    const uint8_t v = get_effective_version(b);
    last_versions[v]++;
    versions.push_back(v);
  }
  const uint64_t bc_height = db.height();
  for (uint64_t h = height + 1; h < bc_height; ++h) {
    add(db.get_block_from_height(h), h);
  }

  return true;
}

bool HardFork::reorganize_from_chain_height(uint64_t height)
{
  if (height == 0)
    return false;
  return reorganize_from_block_height(height - 1);
}

int HardFork::get_voted_fork_index(uint64_t height) const
{
  CRITICAL_REGION_LOCAL(lock);
  uint32_t accumulated_votes = 0;
  for (unsigned int n = heights.size() - 1; n > current_fork_index; --n) {
    uint8_t v = heights[n].version;
    accumulated_votes += last_versions[v];
    if (height >= heights[n].height && accumulated_votes >= vote_threshold) {
      return n;
    }
  }
  return current_fork_index;
}

HardFork::State HardFork::get_state(time_t t) const
{
  CRITICAL_REGION_LOCAL(lock);

  // no hard forks setup yet
  if (heights.size() <= 1)
    return Ready;

  time_t t_last_fork = heights.back().time;
  if (t >= t_last_fork + forked_time)
    return LikelyForked;
  if (t >= t_last_fork + update_time)
    return UpdateNeeded;
  return Ready;
}

HardFork::State HardFork::get_state() const
{
  return get_state(time(NULL));
}

uint8_t HardFork::get(uint64_t height) const
{
  CRITICAL_REGION_LOCAL(lock);
  if (height > db.height()) {
    assert(false);
    return 255;
  }
  return db.get_hard_fork_version(height);
}

uint64_t HardFork::get_start_height(uint8_t version) const
{
  CRITICAL_REGION_LOCAL(lock);
  return db.get_hard_fork_starting_height(version);
}

uint8_t HardFork::get_current_version() const
{
  CRITICAL_REGION_LOCAL(lock);
  return heights[current_fork_index].version;
}

uint8_t HardFork::get_ideal_version() const
{
  CRITICAL_REGION_LOCAL(lock);
  return heights.back().version;
}

bool HardFork::get_voting_info(uint8_t version, uint32_t &window, uint32_t &votes, uint32_t &threshold, uint8_t &voting) const
{
  CRITICAL_REGION_LOCAL(lock);

  const uint8_t current_version = heights[current_fork_index].version;
  const bool enabled = current_version >= version;
  window = versions.size();
  votes = 0;
  for (size_t n = version; n < 256; ++n)
      votes += last_versions[n];
  threshold = vote_threshold;
  assert((votes >= threshold) == enabled);
  voting = heights.back().version;
  return enabled;
}
