#pragma once

#include "containers/BackboneResult.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

/// Is it the end of the search
extern std::atomic<bool> globalEnding;

/// @brief  Mutex for timeout cond
extern std::mutex mutexGlobalEnd;

/// @brief Cond to wait on timeout or wakeup on globalEnding
extern std::condition_variable condGlobalEnd;

/// Final result
extern std::atomic<BackboneResult> finalResult;

/// Backbone literals of the formula (valid when finalResult is COMPLETE)
extern std::vector<int> finalBackbone;

/// To check if painless is using distributed mode
extern std::atomic<bool> dist;