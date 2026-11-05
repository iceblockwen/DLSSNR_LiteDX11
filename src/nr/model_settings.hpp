// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pass.hpp"
namespace lite::nr
{
struct ModelSettings
{
    bool enabled{};
    PassSettings pass;
};
void ConfigureModel(const wchar_t* ini);
ModelSettings GetModelSettings();
void SetModelSettings(const ModelSettings&, bool persist = true);
bool ModelRequested();
bool ModelActive();
bool ProcessingActive();
void PublishPass(const PassStatus&);
void ReportModelWait(const char* reason);
PassStatus LastPass();
UINT64 ModelRevision();
}
