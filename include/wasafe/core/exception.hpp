#pragma once

#include <stdexcept>

#include "wasafe/export.hpp"

namespace WaSafe {

class WASAFE_API Exception : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace WaSafe
