#pragma once

#include <string>
namespace PlaythroughSession {
    unsigned long Generation();
    unsigned long Context();
    bool Allowed(unsigned long epoch);
    std::wstring Headers(unsigned long epoch);
    void BeginLoad(bool newGame = false);
    std::string Character();
    bool NewCharacter();
    void RestoreCharacter(const std::string& id, bool isNew = false);
    void Connect(const std::string& name, long long gamets, const std::string& members = "[]");
    bool TakeNotice(std::string& message);
    bool ValidId(const std::string& value);
    class Scope {
        unsigned long previous;
    public:
        explicit Scope(unsigned long epoch);
        ~Scope();
    };
    // Product transport uses the configured server and reports the actual HTTP status.
    std::string Transport(const std::string& body, unsigned long& status);
}
