// Compile the actual DX12 control-consumption body with bounded state boundaries.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <string>

#include "../../OptiScaler/dlssnr/DlssNr_Status.cpp"
template <class T> struct Option
{
    T value;
    T value_or_default() const { return value; }
};
class Config
{
  public:
    Option<bool> DlssNrEnabled { true };
    Option<int> DlssNrTransfer { 1 };
    Option<float> DlssNrWorkingScale { .75f };
    static Config* Instance()
    {
        static Config cfg;
        return &cfg;
    }
};
bool DlssNrUsesDlssEnlargement(int transfer) { return transfer == 1; }
struct Model
{
    unsigned retries = 0;
    void RetryAfterFailure() { ++retries; }
};
struct Capture
{
    unsigned frames = 0, requests = 0;
    void request(unsigned value)
    {
        frames = value;
        ++requests;
    }
};
struct Owner
{
    struct Nr
    {
        bool reset = true;
        std::array<Model, 2> models;
        std::array<bool, 2> passCreateFailed { true, true };
    } nr;
    DlssNr::ControlRequests controls = DlssNr::ReadControlRequests();
    unsigned releases = 0, retries = 0;
    bool modelRunning = true;
    std::string enlargementStatus;
    Capture captureFrames;
    void ReleaseEnlarger() { ++releases; }
    void RetryAfterFailure()
    {
        ++retries;
        nr.reset = true;
    }
    void ConsumeControls()
    {
#include "consume-controls.inc"
    }
};
unsigned failures = 0, checks = 0;
void Check(bool condition, const char* name)
{
    ++checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    failures += !condition;
}
int main()
{
    Owner first, second;
    first.ConsumeControls();
    Check(first.nr.reset, "initial history reset survives consumption");
    first.nr.reset = second.nr.reset = false;
    first.ConsumeControls();
    Check(!first.nr.reset, "stable controls preserve NR history");
    Config::Instance()->DlssNrEnabled.value = false;
    first.ConsumeControls();
    Config::Instance()->DlssNrEnabled.value = true;
    first.ConsumeControls();
    Check(first.nr.reset, "observed disabled interval resets same-owner history");
    first.nr.reset = false;
    // The UI can toggle both ways between render calls; the owner only sees On.
    const auto before = DlssNr::ReadControlRequests();
    DlssNr::RequestHistoryReset();
    DlssNr::RequestHistoryReset();
    const auto after = DlssNr::ReadControlRequests();
    Check(after.historyGeneration == before.historyGeneration + 2 && after.retryGeneration == before.retryGeneration &&
              after.captureGeneration == before.captureGeneration,
          "actual history producer stamps only history generation");
    first.ConsumeControls();
    second.ConsumeControls();
    Check(first.nr.reset && second.nr.reset, "unseen Off/On resets every existing owner");
    Check(first.retries == 0 && first.nr.models[0].retries == 0 && first.modelRunning,
          "history-only request does not retry or rebuild NR models");
    Check(first.releases == 1, "history-only request does not release active enlarger");
    first.nr.reset = second.nr.reset = false;
    first.ConsumeControls();
    second.ConsumeControls();
    Check(!first.nr.reset && !second.nr.reset, "consumed generation does not reset every frame");
    DlssNr::RetryAfterFailure();
    DlssNr::RequestCapture(7);
    DlssNr::RequestHistoryReset();
    first.ConsumeControls();
    Check(first.nr.reset && first.retries == 1 && first.nr.models[0].retries == 1 && first.nr.models[1].retries == 1 &&
              !first.nr.passCreateFailed[0] && !first.modelRunning,
          "combined retry and history request preserves retry contract");
    Check(first.captureFrames.frames == 7 && first.captureFrames.requests == 1,
          "combined request preserves capture contract");
    first.ConsumeControls();
    Check(first.retries == 1 && first.captureFrames.requests == 1, "retry/capture remain one-shot");
    std::printf("History controls: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
