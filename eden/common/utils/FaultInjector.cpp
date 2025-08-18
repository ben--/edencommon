/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "eden/common/utils/FaultInjector.h"

#include <chrono>
#include <string_view>

#include <folly/Overload.h>
#include <folly/logging/xlog.h>

using folly::SemiFuture;
using folly::Unit;

namespace facebook::eden {

using namespace std::chrono_literals;

FaultInjector::Fault::Fault(
    std::string_view regex,
    FaultBehavior&& b,
    size_t count)
    : keyValueRegex(regex.begin(), regex.end()),
      countRemaining(count),
      behavior(std::move(b)) {}

FaultInjector::FaultInjector(bool enabled) : enabled_{enabled} {}

FaultInjector::~FaultInjector() {
  // If there are any blocked checks still pending on destruction
  // fail them all with an error.
  auto numUnblocked =
      unblockAllImpl(std::runtime_error("FaultInjector destroyed"));
  XLOG_IF(WARN, numUnblocked > 0)
      << "FaultInjector destroyed with " << numUnblocked
      << " blocked check calls still pending";
}

ImmediateFuture<Unit> FaultInjector::checkAsyncImpl(
    std::string_view keyClass,
    std::string_view keyValue) {
  auto behavior = findFault(keyClass, keyValue);
  using RV = ImmediateFuture<Unit>;
  return std::visit(
      folly::overload(
          [&](const Unit&) -> RV { return folly::unit; },
          [&](const FaultInjector::Block&) -> RV {
            XLOGF(DBG1, "block fault hit: {}, {}", keyClass, keyValue);
            return addBlockedFault(keyClass, keyValue);
          },
          [&](const FaultInjector::Delay& delay) -> RV {
            XLOGF(DBG1, "delay fault hit: {}, {}", keyClass, keyValue);
            if (delay.error.has_value()) {
              return folly::futures::sleep(delay.duration)
                  .defer([error = delay.error.value()](auto&&) {
                    error.throw_exception();
                  });
            }
            return folly::futures::sleep(delay.duration);
          },
          [&](const folly::exception_wrapper& error) -> RV {
            XLOGF(DBG1, "error fault hit: {}, {}", keyClass, keyValue);
            return RV{std::move(error)};
          },
          [&](const FaultInjector::Kill&) -> RV {
            XLOGF(DBG1, "kill fault hit: {}, {}", keyClass, keyValue);
            abort();
          }),
      behavior);
}

folly::Try<Unit> FaultInjector::checkTryImpl(
    std::string_view keyClass,
    std::string_view keyValue) {
  return checkAsyncImpl(keyClass, keyValue).getTry();
}

void FaultInjector::checkImpl(
    std::string_view keyClass,
    std::string_view keyValue) {
  auto result = checkTryImpl(keyClass, keyValue);
  if (result.hasException()) {
    result.exception().throw_exception();
  }
}

void FaultInjector::injectError(
    std::string_view keyClass,
    std::string_view keyValueRegex,
    folly::exception_wrapper error,
    size_t count) {
  XLOGF(INFO, "injectError({}, {}, count={})", keyClass, keyValueRegex, count);
  injectFault(keyClass, keyValueRegex, error, count);
}

void FaultInjector::injectBlock(
    std::string_view keyClass,
    std::string_view keyValueRegex,
    size_t count) {
  XLOGF(INFO, "injectBlock({}, {}, count={})", keyClass, keyValueRegex, count);
  injectFault(keyClass, keyValueRegex, Block{}, count);
}

void FaultInjector::injectDelay(
    std::string_view keyClass,
    std::string_view keyValueRegex,
    std::chrono::milliseconds duration,
    size_t count) {
  XLOGF(INFO, "injectDelay({}, {}, count={})", keyClass, keyValueRegex, count);
  injectFault(keyClass, keyValueRegex, Delay{duration}, count);
}

void FaultInjector::injectKill(
    std::string_view keyClass,
    std::string_view keyValueRegex,
    size_t count) {
  XLOGF(INFO, "injectKill({}, {}, count={})", keyClass, keyValueRegex, count);
  injectFault(keyClass, keyValueRegex, Kill{}, count);
}

void FaultInjector::injectDelayedError(
    std::string_view keyClass,
    std::string_view keyValueRegex,
    std::chrono::milliseconds duration,
    folly::exception_wrapper error,
    size_t count) {
  XLOGF(
      INFO,
      "injectDelayedError({}, {}, count={})",
      keyClass,
      keyValueRegex,
      count);
  injectFault(
      keyClass, keyValueRegex, Delay{duration, std::move(error)}, count);
}

void FaultInjector::injectNoop(
    std::string_view keyClass,
    std::string_view keyValueRegex,
    size_t count) {
  XLOGF(INFO, "injectNoop({}, {}, count={})", keyClass, keyValueRegex, count);
  injectFault(keyClass, keyValueRegex, folly::unit, count);
}

void FaultInjector::injectFault(
    std::string_view keyClass,
    std::string_view keyValueRegex,
    FaultBehavior&& behavior,
    size_t count) {
  if (!enabled_) {
    throw std::runtime_error("fault injection is disabled");
  }

  auto state = state_.wlock();
  state->faults[keyClass].emplace_back(
      keyValueRegex, std::move(behavior), count);
}

bool FaultInjector::removeFault(
    std::string_view keyClass,
    std::string_view keyValueRegex) {
  auto state = state_.wlock();

  // Look for any faults matching this key class
  auto classIter = state->faults.find(keyClass);
  if (classIter == state->faults.end()) {
    XLOGF(
        DBG2,
        "removeFault({}, {}) --> no faults defined for class {}",
        keyClass,
        keyValueRegex,
        keyClass);
    return false;
  }

  // Scan all faults in this key class to find a matching regex
  auto& faultVector = classIter->second;
  for (auto iter = faultVector.begin(); iter != faultVector.end(); ++iter) {
    if (iter->keyValueRegex.str() == keyValueRegex) {
      XLOGF(INFO, "removeFault({}, {})", keyClass, keyValueRegex);
      faultVector.erase(iter);
      if (faultVector.empty()) {
        state->faults.erase(classIter);
      }
      return true;
    }
  }

  XLOGF(DBG2, "removeFault({}, {}) --> no match", keyClass, keyValueRegex);
  return false;
}

size_t FaultInjector::unblock(
    std::string_view keyClass,
    std::string_view keyValueRegex) {
  XLOGF(DBG1, "unblock({}, {})", keyClass, keyValueRegex);
  auto matches = extractBlockedChecks(keyClass, keyValueRegex);
  for (auto& match : matches) {
    match.promise.setValue();
  }
  return matches.size();
}

size_t FaultInjector::unblockWithError(
    std::string_view keyClass,
    std::string_view keyValueRegex,
    folly::exception_wrapper error) {
  XLOGF(DBG1, "unblockWithError({}, {})", keyClass, keyValueRegex);
  auto matches = extractBlockedChecks(keyClass, keyValueRegex);
  for (auto& match : matches) {
    match.promise.setException(error);
  }
  return matches.size();
}

size_t FaultInjector::unblockAll() {
  XLOG(DBG1, "unblockAll()");
  return unblockAllImpl(std::nullopt);
}

size_t FaultInjector::unblockAllWithError(folly::exception_wrapper error) {
  XLOG(DBG1, "unblockAllWithError()");
  return unblockAllImpl(std::move(error));
}

FaultInjector::FaultBehavior FaultInjector::findFault(
    std::string_view keyClass,
    std::string_view keyValue) {
  XLOGF(DBG4, "findFault({}, {})", keyClass, keyValue);
  auto state = state_.wlock();

  // Look for any faults matching this key class
  auto classIter = state->faults.find(keyClass);
  if (classIter == state->faults.end()) {
    XLOGF(
        DBG6,
        "findFault({}, {}) --> no faults for class {}",
        keyClass,
        keyValue,
        keyClass);
    return folly::unit;
  }

  // Scan all faults in this key class to find a matching regex
  auto& faultVector = classIter->second;
  for (auto iter = faultVector.begin(); iter != faultVector.end(); ++iter) {
    if (!boost::regex_match(
            keyValue.begin(), keyValue.end(), iter->keyValueRegex)) {
      XLOGF(
          DBG8,
          "findFault({}, {}) --> no match against /{}/",
          keyClass,
          keyValue,
          iter->keyValueRegex.str());
      continue;
    }

    // Found a matching fault
    XLOGF(
        DBG3,
        "findFault({}, {}) --> matched /{}/",
        keyClass,
        keyValue,
        iter->keyValueRegex.str());
    auto behavior = iter->behavior;
    if (iter->countRemaining > 0) {
      --iter->countRemaining;
      if (iter->countRemaining == 0) {
        // This was the last match
        XLOGF(
            DBG1, "fault expired: {}, {}", keyClass, iter->keyValueRegex.str());
        faultVector.erase(iter);
      }
    }
    return behavior;
  }

  XLOGF(DBG6, "findFault({}, {}) --> no matches found", keyClass, keyValue);
  return folly::unit;
}

SemiFuture<Unit> FaultInjector::addBlockedFault(
    std::string_view keyClass,
    std::string_view keyValue) {
  auto state = state_.wlock();

  auto [promise, future] = folly::makePromiseContract<Unit>();
  state->blockedChecks[keyClass].emplace_back(keyValue, std::move(promise));
  return std::move(future);
}

std::vector<FaultInjector::BlockedCheck> FaultInjector::extractBlockedChecks(
    std::string_view keyClass,
    std::string_view keyValueRegex) {
  std::vector<BlockedCheck> results;
  auto state = state_.wlock();

  auto classIter = state->blockedChecks.find(keyClass);
  if (classIter == state->blockedChecks.end()) {
    return results;
  }
  auto& blockedChecks = classIter->second;

  // Walk through the list of blocked calls and extract out the ones that
  // match.  We could use std::remove_if(), but that would still require an
  // extra move of all the matched values at the end.
  //
  // When we find a matching call we move it out to the results array.
  // Everything else we shift forwards in the blockedChecks array to fill in the
  // gaps from the matching calls that we extracted.
  auto writeIter = blockedChecks.begin();
  auto end = blockedChecks.end();
  boost::regex regex(keyValueRegex.begin(), keyValueRegex.end());
  for (auto readIter = blockedChecks.begin(); readIter != end; ++readIter) {
    if (boost::regex_match(readIter->keyValue, regex)) {
      // Move this check out to the result list.
      results.emplace_back(std::move(*readIter));
    } else {
      // This check doesn't match.  Shift it forwards in the array to fill in
      // gaps left by extracted results.  writeIter points to the location in
      // the array where we should shift forwards.
      // It will equal readIter if we haven't extracted any matches yet, so we
      // don't need to move the value anywhere in this case.
      if (readIter != writeIter) {
        *writeIter = std::move(*readIter);
      }
      ++writeIter;
    }
  }
  if (writeIter == blockedChecks.begin()) {
    // We extracted all blocked checks for this key class,
    // so just erase the key class from state->blockedChecks entirely.
    state->blockedChecks.erase(classIter);
  } else {
    // Chop off the end of the array that now has uninitialized values
    // that were moved forwards in the array.
    blockedChecks.erase(writeIter, blockedChecks.end());
  }

  return results;
}

size_t FaultInjector::unblockAllImpl(
    std::optional<folly::exception_wrapper> error) {
  folly::F14NodeMap<std::string, std::vector<BlockedCheck>> blockedChecks;
  {
    auto state = state_.wlock();
    state->blockedChecks.swap(blockedChecks);
  }

  size_t numUnblocked = 0;
  for (auto& classEntry : blockedChecks) {
    for (auto& check : classEntry.second) {
      if (error.has_value()) {
        check.promise.setException(error.value());
      } else {
        check.promise.setValue();
      }
    }
    numUnblocked += classEntry.second.size();
  }
  return numUnblocked;
}

std::vector<std::string> FaultInjector::getBlockedFaults(
    std::string_view keyClass) {
  auto state = state_.rlock();
  std::vector<std::string> results;
  auto classIter = state->blockedChecks.find(keyClass);
  if (classIter == state->blockedChecks.end()) {
    return results;
  }

  for (auto& blockedCheck : classIter->second) {
    results.emplace_back(blockedCheck.keyValue);
  }
  return results;
}

bool FaultInjector::waitUntilBlocked(
    std::string_view keyClass,
    std::chrono::milliseconds timeout) {
  auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < start + timeout &&
         getBlockedFaults(keyClass).size() == 0) {
    /* sleep override */
    std::this_thread::sleep_for(1ms);
  }

  return getBlockedFaults(keyClass).size() != 0;
}

} // namespace facebook::eden
