#include "input.h"
#include "ui/log_window.h"

#include <cstdarg>
#include <cstdio>

void LogWindow_LogGekko(LogLevel, const char*, ...) {}
void LogWindow_LogGekkoV(LogLevel, const char*, va_list) {}

namespace {

int g_checks = 0;
int g_failures = 0;

#define TEST_CHECK(cond, msg)                                                     \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(cond)) {                                                            \
            ++g_failures;                                                         \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);          \
        }                                                                         \
    } while (0)

static unsigned char ByteInput(unsigned value) {
    return static_cast<unsigned char>(value & 0xFFu);
}

static bool HasFrame(const Gekko::GameInput* input, Frame frame) {
    return input && input->frame == frame && input->input_len == 1 && input->input;
}

static unsigned InputByte(const Gekko::GameInput* input) {
    return input && input->input ? input->input[0] : 0xFFFFu;
}

static void TestWrongPredictionInvalidatesFutureFrames() {
    Gekko::InputBuffer buffer;
    buffer.Init(0, 5, 1, 16);

    unsigned char i0 = ByteInput(0x10);
    unsigned char i1 = ByteInput(0x11);
    unsigned char i2 = ByteInput(0x22);
    buffer.AddInput(0, &i0);
    buffer.AddInput(1, &i1);
    buffer.AddInput(2, &i2);

    auto predicted3 = buffer.GetInput(3, true);
    auto predicted4 = buffer.GetInput(4, true);
    auto predicted5 = buffer.GetInput(5, true);
    TEST_CHECK(HasFrame(predicted3.get(), 3) && InputByte(predicted3.get()) == 0x22,
        "frame 3 should initially repeat the previous confirmed input");
    TEST_CHECK(HasFrame(predicted4.get(), 4) && InputByte(predicted4.get()) == 0x22,
        "frame 4 should initially be part of the prediction chain");
    TEST_CHECK(HasFrame(predicted5.get(), 5) && InputByte(predicted5.get()) == 0x22,
        "frame 5 should initially be part of the prediction chain");

    unsigned char real3 = ByteInput(0x33);
    buffer.AddInput(3, &real3);

    TEST_CHECK(buffer.GetLastReceivedFrame() == 3,
        "confirmed mismatched frame should advance last received by one");
    TEST_CHECK(buffer.GetIncorrectPredictionFrame() == 3,
        "first wrong prediction should remain marked for rollback");

    auto confirmed3 = buffer.GetInput(3, false);
    auto staleFuture4 = buffer.GetInput(4, false);
    auto staleFuture5 = buffer.GetInput(5, false);
    TEST_CHECK(HasFrame(confirmed3.get(), 3) && InputByte(confirmed3.get()) == 0x33,
        "mismatched frame should be replaced by the real input");
    TEST_CHECK(!HasFrame(staleFuture4.get(), 4),
        "future frame 4 should be cleared after a mismatch");
    TEST_CHECK(!HasFrame(staleFuture5.get(), 5),
        "future frame 5 should be cleared after a mismatch");

    auto regenerated4 = buffer.GetInput(4, true);
    TEST_CHECK(HasFrame(regenerated4.get(), 4) && InputByte(regenerated4.get()) == 0x33,
        "future prediction should regenerate from corrected frame 3");

    unsigned char real4 = ByteInput(0x44);
    buffer.AddInput(4, &real4);
    TEST_CHECK(buffer.GetLastReceivedFrame() == 4,
        "second confirmed mismatch should also advance sequentially");

    buffer.ClearIncorrectFrames(3);
    TEST_CHECK(buffer.GetIncorrectPredictionFrame() == 4,
        "clearing frame 3 should expose the later wrong prediction");
}

static void TestCorrectPredictionDoesNotMarkRollback() {
    Gekko::InputBuffer buffer;
    buffer.Init(0, 4, 1, 16);

    unsigned char i0 = ByteInput(0x7A);
    buffer.AddInput(0, &i0);

    auto predicted1 = buffer.GetInput(1, true);
    TEST_CHECK(HasFrame(predicted1.get(), 1) && InputByte(predicted1.get()) == 0x7A,
        "frame 1 should predict from frame 0");

    unsigned char real1 = ByteInput(0x7A);
    buffer.AddInput(1, &real1);

    TEST_CHECK(buffer.GetIncorrectPredictionFrame() == Gekko::GameInput::NULL_FRAME,
        "matching predicted input should not mark a rollback");
    TEST_CHECK(buffer.GetLastReceivedFrame() == 1,
        "matching predicted input should advance last received");
}

} // namespace

int main() {
    TestWrongPredictionInvalidatesFutureFrames();
    TestCorrectPredictionDoesNotMarkRollback();

    if (g_failures != 0) {
        std::printf("gekko_input_tests: %d/%d checks failed\n", g_failures, g_checks);
        return 1;
    }

    std::printf("gekko_input_tests: passed (%d checks)\n", g_checks);
    return 0;
}
