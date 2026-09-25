// iox — tests/support/receivers.h: shared stop-counting receiver for cancel/redteam tests.
#pragma once

#include <exception>

#include <iox/core/exec.h>

namespace test_rx {

struct stop_counting_receiver {
    using receiver_concept = stdexec::receiver_tag;
    stdexec::inplace_stop_token token;
    int* stopped = nullptr;
    int* values = nullptr;

    auto get_env() const noexcept {
        return stdexec::env{stdexec::prop{stdexec::get_stop_token, token}};
    }
    template <class... As>
    void set_value(As&&...) && noexcept {
        if (values != nullptr) {
            ++*values;
        }
    }
    void set_error(iox::error) && noexcept {
        if (values != nullptr) {
            ++*values;
        }
    }
    void set_error(std::exception_ptr) && noexcept {
        if (values != nullptr) {
            ++*values;
        }
    }
    void set_stopped() && noexcept {
        if (stopped != nullptr) {
            ++*stopped;
        }
    }
};

}
