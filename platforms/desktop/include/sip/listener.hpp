#ifndef SIP_DESKTOP_LISTENER_HPP
#define SIP_DESKTOP_LISTENER_HPP

#include "types.hpp"
#include <memory>
#include <string>
#include <vector>

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

    virtual void on_message_received(const IncomingMessage &message) {
        (void)message;
    }

    virtual void on_message_status(uint64_t message_id, MessageStatus status, int code, const std::string &reason) {
        (void)message_id; (void)status; (void)code; (void)reason;
    }

    virtual void on_call_stats(std::shared_ptr<Call> call, const CallStats &stats) {
        (void)call; (void)stats;
    }

    virtual void on_transfer_status(std::shared_ptr<Call> call, int code, const std::string &reason) {
        (void)call; (void)code; (void)reason;
    }

    virtual void on_transfer_request(std::shared_ptr<Call> call, const std::string &refer_to_uri, bool has_replaces) {
        (void)call; (void)refer_to_uri; (void)has_replaces;
    }

    virtual void on_call_rejected_busy(const std::string &caller_uri) {
        (void)caller_uri;
    }

    virtual void on_conference_changed(const std::vector<std::shared_ptr<Call>> &calls) {
        (void)calls;
    }
};

} // namespace sip

#endif // SIP_DESKTOP_LISTENER_HPP
