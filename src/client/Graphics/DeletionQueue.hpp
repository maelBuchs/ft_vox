#pragma once

#include <deque>
#include <functional>

class DeletionQueue {
  public:
    void push(std::function<void()>&& function) { _deletors.push_back(std::move(function)); }

    void flush() {
        for (auto it = _deletors.rbegin(); it != _deletors.rend(); ++it) {
            (*it)();
        }
        _deletors.clear();
    }

  private:
    std::deque<std::function<void()>> _deletors;
};
