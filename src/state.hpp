#pragma once

#include "types.hpp"
#include <string>

class StateStore {
public:
    explicit StateStore(const std::string& project_name);

    SessionState load();
    void save(const SessionState& state);
    void clear();
    bool exists() const;

private:
    fs::path state_path_;
};
