#pragma once

#include <nvsdk_ngx_params.h>

// Scoped parameter dimension preservation to prevent native feature creation from corrupting
// caller/shared parameter dictionaries (e.g. Width/Height render vs display dimension leakage).
class ScopedNgxDimensionRestoration
{
  private:
    NVSDK_NGX_Parameter* _parameters = nullptr;
    unsigned int _width = 0;
    unsigned int _height = 0;
    unsigned int _outWidth = 0;
    unsigned int _outHeight = 0;
    bool _hasWidth = false;
    bool _hasHeight = false;
    bool _hasOutWidth = false;
    bool _hasOutHeight = false;

  public:
    explicit ScopedNgxDimensionRestoration(NVSDK_NGX_Parameter* inParams) : _parameters(inParams)
    {
        if (_parameters != nullptr)
        {
            _hasWidth = _parameters->Get(NVSDK_NGX_Parameter_Width, &_width) == NVSDK_NGX_Result_Success;
            _hasHeight = _parameters->Get(NVSDK_NGX_Parameter_Height, &_height) == NVSDK_NGX_Result_Success;
            _hasOutWidth = _parameters->Get(NVSDK_NGX_Parameter_OutWidth, &_outWidth) == NVSDK_NGX_Result_Success;
            _hasOutHeight = _parameters->Get(NVSDK_NGX_Parameter_OutHeight, &_outHeight) == NVSDK_NGX_Result_Success;
        }
    }

    ~ScopedNgxDimensionRestoration()
    {
        if (_parameters != nullptr)
        {
            if (_hasWidth)
                _parameters->Set(NVSDK_NGX_Parameter_Width, _width);
            if (_hasHeight)
                _parameters->Set(NVSDK_NGX_Parameter_Height, _height);
            if (_hasOutWidth)
                _parameters->Set(NVSDK_NGX_Parameter_OutWidth, _outWidth);
            if (_hasOutHeight)
                _parameters->Set(NVSDK_NGX_Parameter_OutHeight, _outHeight);
        }
    }
};
