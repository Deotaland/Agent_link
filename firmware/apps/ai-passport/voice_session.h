#pragma once

#include <cstdint>
#include <cstddef>

namespace voice {

enum class SessionState : uint32_t { Disconnected, Ready, Listening, Thinking, Speaking };

inline SessionState TokenState(uint32_t token) {
    return static_cast<SessionState>(token & 7u);
}

inline uint32_t TokenGeneration(uint32_t token) { return token >> 3; }

// Owned by the button task; other tasks receive its published token.
class Session {
public:
    SessionState State() const { return state_; }
    uint32_t Token() const { return (generation_ << 3) | static_cast<uint32_t>(state_); }

    bool SetConnected(bool ready) {
        if (!ready && state_ != SessionState::Disconnected) {
            Advance();
            state_ = SessionState::Disconnected;
            return true;
        }
        if (ready && state_ == SessionState::Disconnected) {
            Advance();
            state_ = SessionState::Ready;
            return true;
        }
        return false;
    }

    bool Listen() {
        if (state_ == SessionState::Disconnected || state_ == SessionState::Listening) return false;
        Advance();
        state_ = SessionState::Listening;
        return true;
    }

    bool Release(bool send, int64_t now) {
        if (state_ != SessionState::Listening) return false;
        if (send) {
            state_ = SessionState::Thinking;
            waiting_since_ = now;
        } else {
            Cancel();
        }
        return true;
    }

    bool Speak(uint32_t generation) {
        if (generation != generation_ || state_ != SessionState::Thinking) return false;
        state_ = SessionState::Speaking;
        return true;
    }

    bool Drain(uint32_t generation, int64_t now) {
        if (generation != generation_ || state_ != SessionState::Speaking || draining_) return false;
        draining_ = true;
        draining_since_ = now;
        return true;
    }

    bool Finish(uint32_t generation) {
        if (generation != generation_ || state_ != SessionState::Speaking || !draining_) return false;
        Cancel();
        return true;
    }

    void Cancel() {
        Advance();
        if (state_ != SessionState::Disconnected) state_ = SessionState::Ready;
    }

    bool TimedOut(int64_t now) const {
        return state_ == SessionState::Thinking && now - waiting_since_ >= 75LL * 1000 * 1000;
    }

    bool DrainTimedOut(int64_t now) const {
        return state_ == SessionState::Speaking && draining_ && now - draining_since_ >= 30LL * 1000 * 1000;
    }

    static bool AcceptsAudio(uint32_t token) {
        const auto state = TokenState(token);
        return state == SessionState::Thinking || state == SessionState::Speaking;
    }

    static bool CanPlay(uint32_t token, bool screen_rendered, bool connected, bool held, bool formal_requested = false) {
        return AcceptsAudio(token) && screen_rendered && connected && !held &&
            (TokenState(token) == SessionState::Speaking || !formal_requested);
    }

    static bool DrainReady(uint32_t token, bool ended, size_t queued, int64_t last_write, int64_t now) {
        return TokenState(token) == SessionState::Speaking && ended && !queued &&
            (!last_write || now - last_write >= 90000);
    }

private:
    void Advance() {
        draining_ = false;
        generation_ = (generation_ + 1u) & 0x1fffffffu;
        if (!generation_) generation_ = 1;
    }
    SessionState state_ = SessionState::Disconnected;
    uint32_t generation_ = 0;
    int64_t waiting_since_ = 0;
    int64_t draining_since_ = 0;
    bool draining_ = false;
};

// Transport callbacks post commands here; the button task owns the session transitions.
class ReplyControl {
public:
    enum : uint8_t { Start = 1, End = 2, Success = 4, Failure = 8 };

    void Bind(uint32_t token) {
        if (generation_ == TokenGeneration(token)) return;
        generation_ = TokenGeneration(token);
        pending_ = 0;
        started_ = ended_ = terminal_ = false;
    }

    bool Post(uint32_t token, uint8_t status) {
        Bind(token);
        const auto state = TokenState(token);
        if (terminal_ || (state != SessionState::Thinking && state != SessionState::Speaking)) return false;
        if (status == 3) {
            if (ended_ || (state != SessionState::Speaking && !started_)) return false;
            pending_ |= End;
            ended_ = true;
        } else {
            if (state != SessionState::Thinking || started_) return false;
            if (status == 2) { pending_ |= Start; started_ = true; }
            else { pending_ |= status == 0 ? Success : Failure; terminal_ = true; }
        }
        return true;
    }

    uint8_t Take(uint32_t token) {
        Bind(token);
        const uint8_t result = pending_;
        pending_ = 0;
        return result;
    }

    bool Started(uint32_t token) const { return generation_ == TokenGeneration(token) && started_; }
    bool Ended(uint32_t token) const { return generation_ == TokenGeneration(token) && ended_; }

private:
    uint32_t generation_ = 0;
    uint8_t pending_ = 0;
    bool started_ = false, ended_ = false, terminal_ = false;
};

}  // namespace voice
