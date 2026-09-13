#include "../../OptiScaler/dlssnr/DlssNr_NgxDiagnostics.h"
#include <thread>
#include <atomic>
#include <cassert>
std::atomic_uint forwarded{0};
void NVSDK_CONV Original(const char*,NVSDK_NGX_Logging_Level,NVSDK_NGX_Feature){++forwarded;}
int main(){
 NVSDK_NGX_LoggingInfo info{Original,NVSDK_NGX_LOGGING_LEVEL_ON,true};
 DlssNr::NgxDiagnostics::Install(info);
 auto callback=info.LoggingCallback;
 DlssNr::NgxDiagnostics::Install(info); // must not chain to itself
 assert(info.DisableOtherLoggingSinks && info.MinimumLoggingLevel==NVSDK_NGX_LOGGING_LEVEL_VERBOSE);
 callback("idle",NVSDK_NGX_LOGGING_LEVEL_ON,(NVSDK_NGX_Feature)18);
 assert(forwarded==1 && diagnosticLines.empty());
 {
  DlssNr::NgxDiagnostics::Scope trace;
  std::thread worker([&]{callback("worker",NVSDK_NGX_LOGGING_LEVEL_VERBOSE,(NVSDK_NGX_Feature)18);});worker.join();
  assert(diagnosticLines.size()==1 && forwarded==1);
  for(int i=0;i<600;++i)callback("bounded",NVSDK_NGX_LOGGING_LEVEL_VERBOSE,(NVSDK_NGX_Feature)18);
  assert(diagnosticLines.size()==512);
 }
 assert(diagnosticLines.size()==513); // limit notice
 callback("idle again",NVSDK_NGX_LOGGING_LEVEL_VERBOSE,(NVSDK_NGX_Feature)18);
 assert(diagnosticLines.size()==513 && forwarded==1);
 puts("PASS: worker-thread capture, callback preservation, idle filtering and bounded output");
}
