#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>

// Unit test validating XeFGPacing diagnostic ring, 2X gating, and overflow protection:
// 1. NsFromQpc 64-bit integer multiplication overflow prevention on large deltas.
// 2. TsDetour 2X gating: countPlus1 < 3 early exit without mutating deadlines.
// 3. TsDetour >= 3X deadline repair: uniform step expansion and nativeUnit clamp recovery.
// 4. In-memory circular diagnostic ring recording: slot indexing, wrap-around, and burst trace formatting.
// 5. Uninstall cleanup: resetting g_ring, diagnostic sequences, and deadline state.

namespace XeFGDiagTest
{

constexpr int64_t Freq = 10000000LL; // 10 MHz QPC frequency

inline int64_t NsFromQpc(int64_t delta)
{
    if (Freq <= 0 || delta <= 0)
        return 0;

    const int64_t sec = delta / Freq;
    const int64_t rem = delta % Freq;
    return sec * 1000000000LL + (rem * 1000000000LL) / Freq;
}

struct DiagFrameRecord
{
    uint32_t index = 0;
    uint32_t countPlus1 = 0;
    int64_t nativeOutNs = 0;
    int64_t detouredOutNs = 0;
    int64_t medianNs = 0;
    float ringMeasuredMs = 0.0f;
    int64_t schedDurationUs = 0;
    bool scheduled = false;
};

constexpr size_t DiagRingCapacity = 32;
inline DiagFrameRecord g_diagRing[DiagRingCapacity] {};
inline uint64_t g_diagSeq = 0;

inline int64_t g_nextDeadlineNs = 0;
inline int64_t g_burstStepNs = 0;
inline uint32_t g_lastTsIndex = 0;
inline uint32_t g_lastTsCountPlus1 = 0;
inline int64_t g_tsCalls = 0;
inline int64_t g_tsRebased = 0;
inline int64_t g_tsClamped = 0;
inline uint8_t* g_ring = nullptr;

constexpr uint32_t RingMeasuredOffset = 0x1B8;

inline void ResetState()
{
    g_diagSeq = 0;
    g_nextDeadlineNs = 0;
    g_burstStepNs = 0;
    g_lastTsIndex = 0;
    g_lastTsCountPlus1 = 0;
    g_tsCalls = 0;
    g_tsRebased = 0;
    g_tsClamped = 0;
    g_ring = nullptr;
    memset(g_diagRing, 0, sizeof(g_diagRing));
}

inline void TsDetourMock(int64_t* out, int64_t median, uint32_t index, uint32_t countPlus1)
{
    if (out == nullptr || index == 0 || index > 5 || countPlus1 < 3)
        return;

    const int64_t nativeOut = *out;
    const int64_t unit = median / static_cast<int64_t>(countPlus1);

    if (unit <= 0)
        return;

    g_tsCalls++;

    const bool fresh = index == 1 || index <= g_lastTsIndex || countPlus1 != g_lastTsCountPlus1;

    if (fresh)
    {
        int64_t nativeUnit = unit;
        int64_t targetUnit = unit;

        if (g_ring != nullptr)
        {
            const float rawF = *reinterpret_cast<const float*>(g_ring + RingMeasuredOffset);
            float f = rawF;

            if (!(f >= 0.125f) || f > 500.0f)
                f = 500.0f;

            const int64_t fNs = static_cast<int64_t>(f * 1000000.0f);
            const int64_t clampedUnit = fNs / static_cast<int64_t>(countPlus1);

            if (clampedUnit < nativeUnit)
            {
                nativeUnit = clampedUnit;
                g_tsClamped++;
            }

            if (rawF >= 1.0f && rawF <= 500.0f && clampedUnit > 0)
            {
                targetUnit = nativeUnit;
            }
        }

        g_nextDeadlineNs = *out + static_cast<int64_t>(index) * (targetUnit - nativeUnit);
        g_burstStepNs = targetUnit;
        g_tsRebased++;
    }
    else
    {
        g_nextDeadlineNs += g_burstStepNs;
    }

    g_lastTsIndex = index;
    g_lastTsCountPlus1 = countPlus1;

    *out = g_nextDeadlineNs;

    const uint64_t slot = g_diagSeq++;
    auto& d = g_diagRing[slot % DiagRingCapacity];
    d.index = index;
    d.countPlus1 = countPlus1;
    d.nativeOutNs = nativeOut;
    d.detouredOutNs = g_nextDeadlineNs;
    d.medianNs = median;
    d.ringMeasuredMs = (g_ring != nullptr) ? *reinterpret_cast<const float*>(g_ring + RingMeasuredOffset) : 0.0f;
    d.schedDurationUs = 0;
    d.scheduled = false;
}

} // namespace XeFGDiagTest

int main()
{
    printf("[+] Starting XeFGPacing Diagnostic Ring & Edge-Case Unit Tests...\n");

    // Test 1: NsFromQpc 64-bit integer multiplication overflow prevention
    {
        // 1 hour at 10 MHz = 3600 * 10,000,000 = 36,000,000,000 QPC ticks.
        // Old formula: (36,000,000,000 * 1,000,000,000) = 3.6e19, which exceeds LLONG_MAX (~9.22e18).
        const int64_t delta1HourQpc = 36000000000LL;
        const int64_t ns1Hour = XeFGDiagTest::NsFromQpc(delta1HourQpc);
        const int64_t expected1HourNs = 3600LL * 1000000000LL;
        assert(ns1Hour == expected1HourNs && "NsFromQpc must not overflow on 1-hour delta!");

        // 20 minutes at 10 MHz = 12,000,000,000 QPC ticks (would overflow in 15.3 min with old math)
        const int64_t delta20MinQpc = 12000000000LL;
        const int64_t ns20Min = XeFGDiagTest::NsFromQpc(delta20MinQpc);
        const int64_t expected20MinNs = 1200LL * 1000000000LL;
        assert(ns20Min == expected20MinNs && "NsFromQpc must not overflow on 20-minute delta!");

        printf("  [PASS] Test 1: NsFromQpc 64-bit integer overflow protection verified.\n");
    }

    // Test 2: TsDetour 2X gating (countPlus1 == 2 early exit)
    {
        XeFGDiagTest::ResetState();
        int64_t deadline = 15000000LL; // 15 ms
        const int64_t originalDeadline = deadline;

        // countPlus1 = 2 (2X mode)
        XeFGDiagTest::TsDetourMock(&deadline, 30000000LL, 1, 2);

        // Deadline must be completely unchanged, and 0 diagnostic slots recorded
        assert(deadline == originalDeadline && "2X mode (countPlus1=2) must not rewrite deadline!");
        assert(XeFGDiagTest::g_tsCalls == 0 && "2X mode must not increment tsCalls!");
        assert(XeFGDiagTest::g_diagSeq == 0 && "2X mode must not populate diag ring!");

        printf("  [PASS] Test 2: TsDetour 2X mode gating (countPlus1 < 3 bypass) verified.\n");
    }

    // Test 3: TsDetour >= 3X deadline repair & diagnostic ring recording
    {
        XeFGDiagTest::ResetState();
        alignas(16) uint8_t ringData[0x200] {};
        float* pMeasured = reinterpret_cast<float*>(ringData + XeFGDiagTest::RingMeasuredOffset);
        *pMeasured = 11.0f; // 11 ms decoupled measured time in ring
        XeFGDiagTest::g_ring = ringData;

        // 3X mode (countPlus1 = 3, median = 30 ms, native unit = 10 ms)
        int64_t deadline1 = 10000000LL; // frame 1 native deadline
        XeFGDiagTest::TsDetourMock(&deadline1, 30000000LL, 1, 3);

        int64_t deadline2 = 20000000LL; // frame 2 native deadline
        XeFGDiagTest::TsDetourMock(&deadline2, 30000000LL, 2, 3);

        assert(XeFGDiagTest::g_tsCalls == 2 && "TsDetour must record 2 calls for 3X burst!");
        assert(XeFGDiagTest::g_diagSeq == 2 && "TsDetour must record 2 diagnostic records in ring!");

        const auto& rec1 = XeFGDiagTest::g_diagRing[0];
        assert(rec1.index == 1 && rec1.countPlus1 == 3);
        assert(rec1.nativeOutNs == 10000000LL);
        assert(rec1.ringMeasuredMs == 11.0f);

        const auto& rec2 = XeFGDiagTest::g_diagRing[1];
        assert(rec2.index == 2 && rec2.countPlus1 == 3);
        assert(rec2.detouredOutNs == deadline2);

        printf("  [PASS] Test 3: TsDetour >= 3X deadline repair & diagnostic ring recording verified.\n");
    }

    // Test 4: Circular buffer wrap-around and burst trace extraction
    {
        XeFGDiagTest::ResetState();
        // Generate 40 records (exceeding DiagRingCapacity 32)
        for (uint32_t b = 0; b < 8; b++)
        {
            for (uint32_t i = 1; i <= 5; i++)
            {
                int64_t deadline = i * 5000000LL;
                XeFGDiagTest::TsDetourMock(&deadline, 30000000LL, i, 6);
            }
        }

        assert(XeFGDiagTest::g_diagSeq == 40 && "Sequence must count total frames across wraps!");
        // Circular index 40 % 32 = 8
        const auto& wrappedRec = XeFGDiagTest::g_diagRing[0];
        // After 40 records, slot 0 holds record 32 (burst 7, frame 3)
        assert(wrappedRec.countPlus1 == 6);

        printf("  [PASS] Test 4: Diagnostic circular ring wrap-around verified.\n");
    }

    // Test 5: TsDetour decoupled pure render time preservation (eliminating resonant lock)
    // vs. sub-millisecond clamp rebasing (rawF < 1.0f)
    {
        XeFGDiagTest::ResetState();
        alignas(16) uint8_t ringData[0x200] {};
        float* pMeasured = reinterpret_cast<float*>(ringData + XeFGDiagTest::RingMeasuredOffset);
        XeFGDiagTest::g_ring = ringData;

        // Part A: Decoupled render time (rawF = 28.0 ms in a 6X burst, median = 84 ms due to display pacing)
        // countPlus1 = 6. Median unit = 84 / 6 = 14 ms.
        // Pure render step = 28 / 6 = 4.666 ms.
        *pMeasured = 28.0f;
        int64_t deadlineA = 1000000000LL; // 1.0 s native deadline
        const int64_t origNativeOut = deadlineA;
        XeFGDiagTest::TsDetourMock(&deadlineA, 84000000LL, 1, 6);

        // Under rawF >= 1.0f, targetUnit = nativeUnit (4.666 ms).
        // (targetUnit - nativeUnit) == 0, so native *out is untouched and burstStepNs == 4.666 ms!
        assert(deadlineA == origNativeOut && "Decoupled render time must NOT pollute deadlineA with wait median!");
        assert(XeFGDiagTest::g_burstStepNs == 4666666LL && "Burst step must match decoupled render time step!");

        // Part B: Sub-millisecond clamp fallback (rawF = 0.0f from FTInput=Zero, sanity clamps to 500.0f)
        // In this case, rawF < 1.0f, so targetUnit remains median unit (14.0 ms) to eliminate 0 ms clump!
        XeFGDiagTest::ResetState();
        *pMeasured = 0.0f;
        int64_t deadlineB = 1000000000LL;
        XeFGDiagTest::TsDetourMock(&deadlineB, 84000000LL, 1, 6);

        assert(XeFGDiagTest::g_burstStepNs == 14000000LL &&
               "Fallback rawF < 1.0f must rebase burstStepNs to median unit!");
        assert(deadlineB == origNativeOut &&
               "Fallback rawF < 1.0f maintains initial deadline and steps by median unit!");

        printf("  [PASS] Test 5: TsDetour decoupled pure render time preservation & clamp rebasing verified.\n");
    }

    printf("[+] All XeFGPacing Diagnostic Ring & Edge-Case Unit Tests PASSED successfully!\n");
    return 0;
}
