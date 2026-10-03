#include "../OptiScaler/low_latency/input/ConcurrentInputSelection.h"

// Standalone regression checks must stay enabled in Release-mode invocations.
#undef NDEBUG
#include <cassert>
#include <thread>

enum class Input : uint32_t
{
    None,
    Reflex,
    XeLL,
    Count
};
enum class Output : uint32_t
{
    None,
    Reflex,
    XeLL
};

int main()
{
    ConcurrentInputSelection<Input, Output, static_cast<size_t>(Input::Count)> state;
    assert(state.AvailableCount() == 0);
    assert(state.ReadSelection().input == Input::None);
    assert(state.ReadSelection().output == Output::None);
    state.MarkAvailable(Input::Count);
    assert(state.AvailableMask() == 0);
    assert(!state.IsAvailable(Input::Count));

    state.SetInput(Input::Reflex);
    assert(state.ReadSelection().output == Output::None);
    state.SetOutput(Output::XeLL);
    assert(state.ReadSelection().input == Input::Reflex);
    state.SetInput(Input::XeLL);
    assert(state.ReadSelection().output == Output::XeLL);
    state.SetOutput(Output::Reflex);
    assert(state.ReadSelection().input == Input::XeLL);
    state.SetInput(Input::None);
    state.SetOutput(Output::None);

    std::thread reflex(
        [&]
        {
            for (int i = 0; i < 100000; ++i)
            {
                state.MarkAvailable(Input::Reflex);
                state.SetInput(Input::Reflex);
            }
        });
    std::thread xell(
        [&]
        {
            for (int i = 0; i < 100000; ++i)
            {
                state.MarkAvailable(Input::XeLL);
                state.SetOutput(Output::XeLL);
            }
        });
    reflex.join();
    xell.join();

    assert(state.AvailableCount() == 2);
    assert(state.IsAvailable(Input::Reflex));
    assert(state.IsAvailable(Input::XeLL));
    const auto selected = state.ReadSelection();
    assert(selected.input == Input::Reflex);
    assert(selected.output == Output::XeLL);
}
