#include "../boards/ai-passport/voice_session.h"
#include "../components/agent_link/src/voice_queue.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <deque>

using voice::Session;
using voice::SessionState;

static void TestSession() {
    Session session;
    assert(!session.Listen());
    assert(!Session::AcceptsAudio(session.Token()));
    assert(session.SetConnected(true));
    assert(session.State() == SessionState::Ready);
    assert(!Session::AcceptsAudio(session.Token()));
    for (unsigned round = 0; round < 10000; ++round) {
        assert(session.Listen());
        assert(session.State() == SessionState::Listening);
        assert(!Session::AcceptsAudio(session.Token()));
        assert(session.Release(true, 100));
        const uint32_t generation = voice::TokenGeneration(session.Token());
        assert(Session::AcceptsAudio(session.Token()));
        assert(Session::CanPlay(session.Token(), true, true, false));
        assert(!Session::CanPlay(session.Token(), true, true, false, true));
        assert(!session.Speak(generation - 1));
        assert(session.Speak(generation));
        assert(!Session::DrainReady(session.Token(), false, 0, 0, 1000000));
        assert(!Session::DrainReady(session.Token(), true, 640, 0, 1000000));
        assert(Session::DrainReady(session.Token(), true, 0, 0, 1000000));
        assert(!Session::DrainReady(session.Token(), true, 0, 1000000, 1089999));
        assert(Session::DrainReady(session.Token(), true, 0, 1000000, 1090000));
        assert(Session::CanPlay(session.Token(), true, true, false));
        assert(!Session::CanPlay(session.Token(), false, true, false));
        assert(!Session::CanPlay(session.Token(), true, false, false));
        assert(!Session::CanPlay(session.Token(), true, true, true));
        assert(!session.Finish(generation));
        assert(!session.TimedOut(100000000000LL));
        assert(!session.DrainTimedOut(100000000000LL));
        assert(session.Drain(generation, 200));
        assert(!session.DrainTimedOut(200 + 29999999));
        assert(session.DrainTimedOut(200 + 30000000));
        assert(session.Finish(generation));
        assert(session.State() == SessionState::Ready);
        assert(!Session::AcceptsAudio(session.Token()));
        assert(!session.Finish(generation));
    }
    assert(session.Listen());
    assert(session.Release(false, 0));
    assert(session.State() == SessionState::Ready);
    assert(session.Listen());
    assert(session.Release(true, 100));
    assert(!session.TimedOut(100 + 74999999));
    assert(session.TimedOut(100 + 75000000));
    uint32_t old = voice::TokenGeneration(session.Token());
    assert(session.Listen());
    assert(!session.Finish(old));
    assert(!Session::AcceptsAudio(session.Token()));
    assert(session.Release(true, 0));
    old = voice::TokenGeneration(session.Token());
    assert(session.Speak(old));
    assert(session.SetConnected(false));
    assert(!session.Finish(old));
    assert(!Session::CanPlay(session.Token(), true, true, false));
    assert(session.SetConnected(true));
    assert(session.Listen());
    session.Cancel();
    assert(session.State() == SessionState::Ready);
}

static void TestReplyControl() {
    voice::ReplyControl replies;
    Session session;
    assert(!replies.Post(session.Token(), 2));
    assert(session.SetConnected(true));
    assert(!replies.Post(session.Token(), 2));
    assert(!replies.Post(session.Token(), 3));
    for (unsigned round = 0; round < 10000; ++round) {
        assert(session.Listen());
        assert(!replies.Post(session.Token(), 0));
        assert(!replies.Post(session.Token(), 2));
        assert(session.Release(true, 100));
        const uint32_t thinking = session.Token();
        const uint32_t generation = voice::TokenGeneration(thinking);
        for (unsigned prelude = 0; prelude < 4; ++prelude) {
            assert(replies.Take(thinking) == 0);
            assert(session.State() == SessionState::Thinking);
            assert(Session::CanPlay(thinking, true, true, false, replies.Started(thinking)));
            assert(!session.Finish(generation));
            assert(!replies.Post(thinking, 3));
        }
        assert(replies.Post(thinking, 2));
        assert(!Session::CanPlay(thinking, true, true, false, replies.Started(thinking)));
        assert(!replies.Post(thinking, 2));
        assert(!replies.Post(thinking, 0));
        assert(!replies.Post(thinking, 1));
        assert(replies.Take(thinking) == voice::ReplyControl::Start);
        assert(session.Speak(generation));
        for (unsigned pause = 0; pause < 5; ++pause) {
            assert(replies.Take(session.Token()) == 0);
            assert(session.State() == SessionState::Speaking);
            assert(!session.TimedOut(100000000000LL));
            assert(!session.Finish(generation));
        }
        assert(replies.Post(session.Token(), 3));
        assert(!replies.Post(session.Token(), 3));
        assert(replies.Ended(session.Token()));
        assert(replies.Take(session.Token()) == voice::ReplyControl::End);
        assert(session.Drain(generation, 200));
        assert(session.State() == SessionState::Speaking);
        assert(Session::CanPlay(session.Token(), true, true, false, true));
        assert(session.Finish(generation));
        assert(replies.Take(session.Token()) == 0);
        assert(!replies.Started(session.Token()));
        assert(!replies.Ended(session.Token()));
    }
    assert(session.Listen());
    assert(session.Release(true, 0));
    const uint32_t token = session.Token(), generation = voice::TokenGeneration(token);
    assert(replies.Post(token, 2));
    assert(replies.Post(token, 3));
    assert(replies.Take(token) == (voice::ReplyControl::Start | voice::ReplyControl::End));
    assert(session.Speak(generation));
    assert(session.Drain(generation, 0));
    assert(session.Finish(generation));
    for (uint8_t status : {uint8_t(0), uint8_t(1), uint8_t(9)}) {
        assert(session.Listen());
        assert(session.Release(true, 0));
        assert(replies.Post(session.Token(), status));
        assert(!replies.Post(session.Token(), 2));
        assert(replies.Take(session.Token()) == (status == 0 ? voice::ReplyControl::Success : voice::ReplyControl::Failure));
        session.Cancel();
        assert(session.State() == SessionState::Ready);
    }
    assert(session.Listen());
    assert(session.Release(true, 0));
    assert(replies.Post(session.Token(), 2));
    assert(session.SetConnected(false));
    assert(replies.Take(session.Token()) == 0);
    assert(!Session::CanPlay(session.Token(), true, false, false));
}

static void TestQueue() {
    agentlink::VoiceQueue<8192> queue;
    std::array<uint8_t, 8192> input{};
    std::array<uint8_t, 8192> output{};
    for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<uint8_t>(i);
    assert(queue.Push(input.data(), input.size()));
    assert(!queue.Push(input.data(), 2));
    assert(queue.Size() == input.size());
    assert(queue.Pop(output.data(), 640) == 640);
    assert(queue.Push(input.data(), 640));
    assert(queue.Pop(output.data(), output.size()) == output.size());
    for (size_t i = 0; i < output.size() - 640; ++i) assert(output[i] == input[i + 640]);
    for (size_t i = 0; i < 640; ++i) assert(output[output.size() - 640 + i] == input[i]);
    assert(queue.Push(input.data(), 640));
    queue.Clear();
    assert(queue.Pop(output.data(), output.size()) == 0);
    assert(queue.Push(input.data() + 1, 640));
    assert(queue.Pop(output.data(), 640) == 640);
    for (size_t i = 0; i < 640; ++i) assert(output[i] == input[i + 1]);

    std::deque<uint8_t> expected;
    uint32_t random = 1;
    for (unsigned i = 0; i < 50000; ++i) {
        random = random * 1664525u + 1013904223u;
        const size_t bytes = 1 + ((random >> 8) % 2048);
        if (random & 1u) {
            const bool fits = bytes <= input.size() - expected.size();
            assert(queue.Push(input.data(), bytes) == fits);
            if (fits) for (size_t n = 0; n < bytes; ++n) expected.push_back(input[n]);
        } else {
            const size_t want = bytes < expected.size() ? bytes : expected.size();
            assert(queue.Pop(output.data(), bytes) == want);
            for (size_t n = 0; n < want; ++n) {
                assert(output[n] == expected.front());
                expected.pop_front();
            }
        }
        assert(queue.Size() == expected.size());
    }
}

int main() {
    TestSession();
    TestReplyControl();
    TestQueue();
    std::puts("PASS: preludes stay Thinking; commands control Speaking/drain; stale events, gates and bounded FIFO");
}
