#pragma once
#include <string>
namespace DlssNrNative {
void* WrapNvapi(unsigned id,void* original);
void SetEnabled(bool enabled);
void SetPrecision(unsigned precision);
bool IsActive();
std::string Status();
}
