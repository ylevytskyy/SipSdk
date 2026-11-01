#ifndef SIP_DESKTOP_LISTENER_HPP
#define SIP_DESKTOP_LISTENER_HPP

#include "types.hpp"
#include <memory>
#include <string>

namespace sip {

class Call;

class IClientListener {
public:
    virtual ~IClientListener() = default;

    virtual void on_registration_state(RegistrationState state, int code, const std::string &reason) {
        (void)state; (void)code; (void)reason;
    }

    virtual void on_incoming_call(std::shared_ptr<Call> call) {
        (void)call;
    }

    virtual void on_call_state(std::shared_ptr<Call> call, CallState state, int code, const std::string &reason) {
        (void)call; (void)state; (void)code; (void)reason;
    }

    virtual void on_call_media(std::shared_ptr<Call> call, const CallMediaInfo &media) {
        (void)call; (void)media;
    }

    virtual void on_dtmf_digit(std::shared_ptr<Call> call, const std::string &digit) {
        (void)call; (void)digit;
    }
};

} // namespace sip

#endif // SIP_DESKTOP_LISTENER_HPP
