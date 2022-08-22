//===- CDependencies.cpp - Dependency Discovery C Interface ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements the dependency discovery interface. It provides a C library for
// the functionality that clang-scan-deps provides.
//
//===----------------------------------------------------------------------===//

#include "CXString.h"

#include "clang-c/Dependencies.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningService.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningTool.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningWorker.h"
#include "llvm/CAS/CachingOnDiskFileSystem.h"

using namespace clang;
using namespace clang::tooling::dependencies;

DEFINE_SIMPLE_CONVERSION_FUNCTIONS(DependencyScanningService,
                                   CXDependencyScannerService)
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(DependencyScanningWorker,
                                   CXDependencyScannerWorker)

inline ScanningOutputFormat unwrap(CXDependencyMode Format) {
  switch (Format) {
  case CXDependencyMode_Flat:
    return ScanningOutputFormat::Make;
  case CXDependencyMode_Full:
    return ScanningOutputFormat::Full;
  }
}

static CXOutputKind wrap(ModuleOutputKind MOK) {
  switch (MOK) {
  case ModuleOutputKind::ModuleFile:
    return CXOutputKind_ModuleFile;
  case ModuleOutputKind::DependencyFile:
    return CXOutputKind_Dependencies;
  case ModuleOutputKind::DependencyTargets:
    return CXOutputKind_DependenciesTarget;
  case ModuleOutputKind::DiagnosticSerializationFile:
    return CXOutputKind_SerializedDiagnostics;
  }
  llvm::report_fatal_error("unhandled ModuleOutputKind");
}

void clang_experimental_ModuleDependencySet_dispose(
    CXModuleDependencySet *MDS) {
  for (int I = 0; I < MDS->Count; ++I) {
    CXModuleDependency &MD = MDS->Modules[I];
    clang_disposeString(MD.Name);
    clang_disposeString(MD.ContextHash);
    clang_disposeString(MD.ModuleMapPath);
    clang_disposeStringSet(MD.FileDeps);
    clang_disposeStringSet(MD.ModuleDeps);
  }
  delete[] MDS->Modules;
  delete MDS;
}

CXDependencyScannerService
clang_experimental_DependencyScannerService_create_v0(CXDependencyMode Format) {
  // FIXME: Pass default CASOpts and nullptr as CachingOnDiskFileSystem now.
  CASOptions CASOpts;
  IntrusiveRefCntPtr<llvm::cas::CachingOnDiskFileSystem> FS;
  return wrap(new DependencyScanningService(
      ScanningMode::DependencyDirectivesScan, unwrap(Format), CASOpts, FS,
      /*ReuseFilemanager=*/false));
}

void clang_experimental_DependencyScannerService_dispose_v0(
    CXDependencyScannerService Service) {
  delete unwrap(Service);
}

void clang_experimental_FileDependencies_dispose(CXFileDependencies *ID) {
  clang_disposeString(ID->ContextHash);
  clang_disposeStringSet(ID->FileDeps);
  clang_disposeStringSet(ID->ModuleDeps);
  delete ID;
}

void clang_experimental_FileDependencies_dispose_v4(CXFileDependencies_v4 *FD) {
  for (size_t I = 0; I < FD->NumCommands; ++I) {
    clang_disposeString(FD->Commands[I].ContextHash);
    clang_disposeStringSet(FD->Commands[I].FileDeps);
    clang_disposeStringSet(FD->Commands[I].ModuleDeps);
    clang_disposeString(FD->Commands[I].Executable);
    clang_disposeStringSet(FD->Commands[I].BuildArguments);
  }
  delete[] FD->Commands;
  delete FD;
}

CXDependencyScannerWorker clang_experimental_DependencyScannerWorker_create_v0(
    CXDependencyScannerService Service) {
  return wrap(new DependencyScanningWorker(
      *unwrap(Service), llvm::vfs::createPhysicalFileSystem()));
}

void clang_experimental_DependencyScannerWorker_dispose_v0(
    CXDependencyScannerWorker Worker) {
  delete unwrap(Worker);
}

using HandleFullDepsCallback = llvm::function_ref<void(FullDependencies)>;

static bool getFullDependencies(
    DependencyScanningWorker *Worker, ArrayRef<std::string> Compilation,
    const char *WorkingDirectory, CXModuleDiscoveredCallback *MDC,
    void *Context, CXString *error, LookupModuleOutputCallback LookupOutput,
    bool DeprecatedDriverCommand, llvm::Optional<StringRef> ModuleName,
    HandleFullDepsCallback HandleFullDeps) {
  llvm::StringSet<> AlreadySeen;
  FullDependencyConsumer Consumer(AlreadySeen, LookupOutput);
  llvm::Error Result = Worker->computeDependencies(
      WorkingDirectory, Compilation, Consumer, ModuleName);

  if (Result) {
    *error = cxstring::createDup(llvm::toString(std::move(Result)));
    return false;
  }

  FullDependenciesResult FDR;
  if (DeprecatedDriverCommand)
    FDR = Consumer.getFullDependenciesLegacyDriverCommand(Compilation);
  else
    FDR = Consumer.takeFullDependencies();

  if (!FDR.DiscoveredModules.empty()) {
    CXModuleDependencySet *MDS = new CXModuleDependencySet;
    MDS->Count = FDR.DiscoveredModules.size();
    MDS->Modules = new CXModuleDependency[MDS->Count];
    for (int I = 0; I < MDS->Count; ++I) {
      CXModuleDependency &M = MDS->Modules[I];
      const ModuleDeps &MD = FDR.DiscoveredModules[I];
      M.Name = cxstring::createDup(MD.ID.ModuleName);
      M.ContextHash = cxstring::createDup(MD.ID.ContextHash);
      M.ModuleMapPath = cxstring::createDup(MD.ClangModuleMapFile);
      M.FileDeps = cxstring::createSet(MD.FileDeps);
      std::vector<std::string> Modules;
      for (const ModuleID &MID : MD.ClangModuleDeps)
        Modules.push_back(MID.ModuleName + ":" + MID.ContextHash);
      M.ModuleDeps = cxstring::createSet(Modules);
      M.BuildArguments = cxstring::createSet(MD.getCanonicalCommandLine());
    }
    MDC(Context, MDS);
  }

  HandleFullDeps(std::move(FDR.FullDeps));
  return true;
}

static bool
getFileDependencies(CXDependencyScannerWorker W, int argc,
                    const char *const *argv, const char *WorkingDirectory,
                    CXModuleDiscoveredCallback *MDC, void *Context,
                    CXString *error, LookupModuleOutputCallback LookupOutput,
                    bool DeprecatedDriverCommand,
                    llvm::Optional<StringRef> ModuleName,
                    HandleFullDepsCallback HandleFullDeps) {
  if (!W) {
    if (error)
      *error = cxstring::createDup("missing CXDependencyScannerWorker");
    return false;
  }
  if (argc < 2 || !argv) {
    if (error)
      *error = cxstring::createDup("invalid argc or argv");
    return false;
  }
  if (error)
    *error = cxstring::createEmpty();

  DependencyScanningWorker *Worker = unwrap(W);

  if (Worker->getFormat() != ScanningOutputFormat::Full) {
    if (error)
      *error = cxstring::createDup("unsupported CXDependencyMode for this API");
    return false;
  }

  std::vector<std::string> Compilation{argv, argv + argc};

  return getFullDependencies(Worker, Compilation, WorkingDirectory, MDC,
                             Context, error, LookupOutput,
                             DeprecatedDriverCommand, ModuleName,
                             HandleFullDeps);
}

namespace {
class OutputLookup {
public:
  OutputLookup(void *MLOContext, CXModuleLookupOutputCallback *MLO)
      : MLOContext(MLOContext), MLO(MLO) {}
  std::string lookupModuleOutput(const ModuleID &ID, ModuleOutputKind MOK);

private:
  std::unordered_map<ModuleID, std::string, ModuleIDHasher> PCMPaths;
  void *MLOContext;
  CXModuleLookupOutputCallback *MLO;
};
} // end anonymous namespace

CXFileDependencies *
clang_experimental_DependencyScannerWorker_getFileDependencies_v3(
    CXDependencyScannerWorker W, int argc, const char *const *argv,
    const char *ModuleName, const char *WorkingDirectory, void *MDCContext,
    CXModuleDiscoveredCallback *MDC, void *MLOContext,
    CXModuleLookupOutputCallback *MLO, unsigned, CXString *error) {
  OutputLookup OL(MLOContext, MLO);
  auto LookupOutputs = [&](const ModuleID &ID, ModuleOutputKind MOK) {
    return OL.lookupModuleOutput(ID, MOK);
  };
  CXFileDependencies *FDeps = nullptr;
  bool Success = getFileDependencies(
      W, argc, argv, WorkingDirectory, MDC, MDCContext, error, LookupOutputs,
      /*DeprecatedDriverCommand=*/true,
      ModuleName ? Optional<StringRef>(ModuleName) : None, [&](FullDependencies FD) {
    assert(!FD.DriverCommandLine.empty());
    std::vector<std::string> Modules;
    for (const ModuleID &MID : FD.ClangModuleDeps)
      Modules.push_back(MID.ModuleName + ":" + MID.ContextHash);
    FDeps = new CXFileDependencies;
    FDeps->ContextHash = cxstring::createDup(FD.ID.ContextHash);
    FDeps->FileDeps = cxstring::createSet(FD.FileDeps);
    FDeps->ModuleDeps = cxstring::createSet(Modules);
    FDeps->BuildArguments = cxstring::createSet(FD.DriverCommandLine);
  });
  assert(!Success || FDeps);
  return FDeps;
}

CXFileDependencies_v4 *
clang_experimental_DependencyScannerWorker_getFileDependencies_v4(
    CXDependencyScannerWorker W, int argc, const char *const *argv,
    const char *ModuleName, const char *WorkingDirectory, void *MDCContext,
    CXModuleDiscoveredCallback *MDC, void *MLOContext,
    CXModuleLookupOutputCallback *MLO, unsigned, CXString *error) {
  OutputLookup OL(MLOContext, MLO);
  auto LookupOutputs = [&](const ModuleID &ID, ModuleOutputKind MOK) {
    return OL.lookupModuleOutput(ID, MOK);
  };
  CXFileDependencies_v4 *Result = nullptr;
  bool Success = getFileDependencies(
      W, argc, argv, WorkingDirectory, MDC, MDCContext, error, LookupOutputs,
      /*DeprecatedDriverCommand=*/false,
      ModuleName ? Optional<StringRef>(ModuleName) : None, [&](FullDependencies FD) {
    assert(FD.DriverCommandLine.empty());
    std::vector<std::string> Modules;
    for (const ModuleID &MID : FD.ClangModuleDeps)
      Modules.push_back(MID.ModuleName + ":" + MID.ContextHash);
    auto *Commands = new CXTranslationUnitCommand[FD.Commands.size()];
    for (size_t I = 0, E = FD.Commands.size(); I < E; ++I) {
      Commands[I].ContextHash = cxstring::createDup(FD.ID.ContextHash);
      Commands[I].FileDeps = cxstring::createSet(FD.FileDeps);
      Commands[I].ModuleDeps = cxstring::createSet(Modules);
      Commands[I].Executable = cxstring::createDup(FD.Commands[I]->getExecutable());
      Commands[I].BuildArguments = cxstring::createSet(FD.Commands[I]->getArguments());
      bzero(&Commands[I].Reserved, sizeof(uintptr_t) * 4);
    }
    Result = new CXFileDependencies_v4{FD.Commands.size(), Commands};
  });
  assert(!Success || Result);
  return Result;
}

static std::string lookupModuleOutput(const ModuleID &ID, ModuleOutputKind MOK,
                                      void *MLOContext,
                                      CXModuleLookupOutputCallback *MLO) {
  SmallVector<char, 256> Buffer(256);
  size_t Len = MLO(MLOContext, ID.ModuleName.c_str(), ID.ContextHash.c_str(),
                   wrap(MOK), Buffer.data(), Buffer.size());
  if (Len > Buffer.size()) {
    Buffer.resize(Len);
    Len = MLO(MLOContext, ID.ModuleName.c_str(), ID.ContextHash.c_str(),
              wrap(MOK), Buffer.data(), Buffer.size());
  }
  return std::string(Buffer.begin(), Len);
}

std::string OutputLookup::lookupModuleOutput(const ModuleID &ID,
                                             ModuleOutputKind MOK) {
  if (MOK != ModuleOutputKind::ModuleFile)
    return ::lookupModuleOutput(ID, MOK, MLOContext, MLO);
  // PCM paths are looked up repeatedly, so cache them.
  auto PCMPath = PCMPaths.insert({ID, ""});
  if (PCMPath.second)
    PCMPath.first->second = ::lookupModuleOutput(ID, MOK, MLOContext, MLO);
  return PCMPath.first->second;
}
