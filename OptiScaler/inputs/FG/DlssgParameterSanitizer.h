#pragma once

#include <nvsdk_ngx_params.h>
#include <cstring>

class DlssgParameterSanitizer : public NVSDK_NGX_Parameter
{
  private:
    NVSDK_NGX_Parameter* _inner = nullptr;
    bool _filterOutputReal = false;

    static bool IsOutputRealKey(const char* name)
    {
        return name != nullptr && std::strcmp(name, "DLSSG.OutputReal") == 0;
    }

  public:
    DlssgParameterSanitizer(NVSDK_NGX_Parameter* inner, bool filterOutputReal)
        : _inner(inner), _filterOutputReal(filterOutputReal)
    {
    }

    bool IsFilteringOutputReal() const { return _filterOutputReal; }
    NVSDK_NGX_Parameter* GetInner() const { return _inner; }

    void Set(const char* InName, unsigned long long InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    void Set(const char* InName, float InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    void Set(const char* InName, double InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    void Set(const char* InName, unsigned int InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    void Set(const char* InName, int InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    void Set(const char* InName, ID3D11Resource* InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    void Set(const char* InName, ID3D12Resource* InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    void Set(const char* InName, void* InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    NVSDK_NGX_Result Get(const char* InName, unsigned long long* OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        if (_filterOutputReal && IsOutputRealKey(InName))
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;

        return _inner->Get(InName, OutValue);
    }

    NVSDK_NGX_Result Get(const char* InName, float* OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        return _inner->Get(InName, OutValue);
    }

    NVSDK_NGX_Result Get(const char* InName, double* OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        return _inner->Get(InName, OutValue);
    }

    NVSDK_NGX_Result Get(const char* InName, unsigned int* OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        return _inner->Get(InName, OutValue);
    }

    NVSDK_NGX_Result Get(const char* InName, int* OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        return _inner->Get(InName, OutValue);
    }

    NVSDK_NGX_Result Get(const char* InName, ID3D11Resource** OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        return _inner->Get(InName, OutValue);
    }

    NVSDK_NGX_Result Get(const char* InName, ID3D12Resource** OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        if (_filterOutputReal && IsOutputRealKey(InName))
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;

        return _inner->Get(InName, OutValue);
    }

    NVSDK_NGX_Result Get(const char* InName, void** OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        if (_filterOutputReal && IsOutputRealKey(InName))
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;

        return _inner->Get(InName, OutValue);
    }

    void Reset() override
    {
        if (_inner != nullptr)
            _inner->Reset();
    }
};

inline bool ShouldFilterDlssgOutputReal(NVSDK_NGX_Parameter* InParameters)
{
    if (InParameters == nullptr)
        return false;

    ID3D12Resource* d3d12Resource = nullptr;
    if (InParameters->Get("DLSSG.OutputReal", &d3d12Resource) == NVSDK_NGX_Result_Success)
        return d3d12Resource == nullptr;

    void* voidPtr = nullptr;
    if (InParameters->Get("DLSSG.OutputReal", &voidPtr) == NVSDK_NGX_Result_Success)
        return voidPtr == nullptr;

    return false;
}
