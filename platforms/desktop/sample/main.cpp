#include <sip/desktop.hpp>
#include <iostream>
#include <thread>
#include <chrono>

class SampleListener : public sip::IClientListener {
public:
    void on_registration_state(sip::RegistrationState state, int code, const std::string &reason) override {
        std::cout << "[Sample] Registration state changed: " << sip::to_string(state)
                  << " (code=" << code << ", reason=" << reason << ")" << std::endl;
    }

    void on_incoming_call(std::shared_ptr<sip::Call> call) override {
        std::cout << "[Sample] Incoming call from: " << call->get_remote_uri() << std::endl;
    }

    void on_call_state(std::shared_ptr<sip::Call> call, sip::CallState state, int code, const std::string &reason) override {
        std::cout << "[Sample] Call " << call->get_id() << " state: " << sip::to_string(state)
                  << " (code=" << code << ", reason=" << reason << ")" << std::endl;
    }
};

int main(int argc, char **argv) {
    std::cout << "SipSdk Desktop Sample Application (Version " << sip::Client::get_version() << ")" << std::endl;

    std::string user = (argc > 1) ? argv[1] : "1001";
    std::string domain = (argc > 2) ? argv[2] : "127.0.0.1";
    std::string pwd = (argc > 3) ? argv[3] : "test1001";

    auto client = std::make_shared<sip::Client>(user, domain);
    client->set_password(pwd);
    client->set_listener(std::make_shared<SampleListener>());

    std::cout << "Starting client for " << client->aor() << "..." << std::endl;
    int rc = client->start();
    if (rc != 0) {
        std::cerr << "Failed to start client (rc=" << rc << ")" << std::endl;
        return 1;
    }

    std::cout << "Client started successfully. Stopping..." << std::endl;
    client->stop();
    std::cout << "Sample completed successfully." << std::endl;
    return 0;
}
