// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/integrations/pjrt/common/dylib_platform.h"

#include <optional>
#include <string>

#include "iree/base/internal/file_io.h"
#include "iree/base/internal/path.h"
#include "iree/compiler/API2/Stub/Loader.h"

namespace iree::pjrt {

namespace {

std::optional<std::string> InitializeCompilerForProcess(
    const std::string& library_path) {
  if (!ireeCompilerLoadLibrary(library_path.c_str())) {
    return {};
  }

  ireeCompilerGlobalInitialize(/*initializeCommandLine=*/false);

  return library_path;
}

// Since we delay load the compiler, it can only be done once per process.
// First one to do it wins. Returns the path of the loaded compiler or
// empty if it could not be loaded.
std::optional<std::string> LoadCompilerStubOnce(
    const std::string& library_path) {
  static std::optional<std::string> loaded_path =
      ([&]() -> std::optional<std::string> {
        return InitializeCompilerForProcess(library_path);
      })();
  return loaded_path;
}

}  // namespace

iree_status_t DylibPlatform::SubclassInitialize() {
  // Fallback config to environment.
  config_vars().EnableEnvFallback("IREE_PJRT_");

  // Just a vanilla logger for now.
  logger_ = std::make_unique<Logger>();

  // Compute library path.
  auto library_path = GetCompilerLibraryPath();
  if (!library_path) {
    return iree_make_status(
        IREE_STATUS_UNAVAILABLE,
        "the compiler library could not be found. It can be configured via the "
        "'COMPILER_LIBRARY_PATH' config var ('IREE_PJRT_COMPILER_LIBRARY_PATH' "
        "env var)");
  }

  // Process once initialization of the shared library.
  auto loaded_compiler = LoadCompilerStubOnce(*library_path);
  if (!loaded_compiler) {
    logger().error("Could not initialize compiler shared library");
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "unable to locate IREE compiler shared library: %s",
                            library_path->c_str());
  }
  {
    std::string message("Using IREE compiler binary: ");
    message.append(*loaded_compiler);
    logger().debug(message);
  }

  // And initialize the compiler.
  compiler_ = std::make_unique<InprocessCompiler>();

  // Initialize the artifact dumper.
  auto artifact_path_callback = [this]() -> std::optional<std::string> {
    return config_vars().Lookup("SAVE_ARTIFACTS");
  };
  // TODO: Use a config key like "SAVE_ALL" to control all artifact saving.
  artifact_dumper_ = std::make_unique<FilesArtifactDumper>(
      logger(), artifact_path_callback, /*retain_all=*/false);

  return iree_ok_status();
}

std::optional<std::string> DylibPlatform::GetHomeDir() {
  auto found = config_vars().Lookup("HOME_DIR");
  if (found) {
    return *found;
  }
  return {};
}

std::optional<std::string> DylibPlatform::GetBinaryDir() {
  auto found_explicit = config_vars().Lookup("BIN_DIR");
  if (found_explicit) {
    return *found_explicit;
  }

  // Try to compute from home.
  auto home_dir = GetHomeDir();
  if (!home_dir) return {};

  // The development tree uses 'tools' unfortunately. Try both.
  std::array<const char*, 2> local_names = {"bin", "tools"};
  for (const char* local_name : local_names) {
    char* path;
    auto status = iree_file_path_join(
        iree_make_string_view(home_dir->data(), home_dir->size()),
        iree_make_cstring_view(local_name), iree_allocator_system(), &path);
    if (!iree_status_is_ok(status)) continue;

    std::string existing_path(path);
    iree_allocator_free(iree_allocator_system(), path);
    status = iree_file_exists(path);
    if (iree_status_is_ok(status)) return existing_path;
  }

  return {};
}

std::optional<std::string> DylibPlatform::GetLibraryDir() {
  auto found_explicit = config_vars().Lookup("LIB_DIR");
  if (found_explicit) {
    return *found_explicit;
  }

  // Try to compute from home.
  auto home_dir = GetHomeDir();
  if (!home_dir) return {};

  std::array<const char*, 2> local_names = {"lib", "lib64"};
  for (const char* local_name : local_names) {
    char* path;
    auto status = iree_file_path_join(
        iree_make_string_view(home_dir->data(), home_dir->size()),
        iree_make_cstring_view(local_name), iree_allocator_system(), &path);
    if (!iree_status_is_ok(status)) continue;

    std::string existing_path(path);
    iree_allocator_free(iree_allocator_system(), path);
    status = iree_file_exists(path);
    if (iree_status_is_ok(status)) return existing_path;
  }

  return {};
}

std::optional<std::string> DylibPlatform::GetCompilerLibraryPath() {
  auto found_explicit = config_vars().Lookup("COMPILER_LIB_PATH");
  if (found_explicit) {
    return *found_explicit;
  }

  // Try to compute from lib dir.
  auto lib_dir = GetLibraryDir();
  if (!lib_dir) return {};

  char* path;
  auto status = iree_file_path_join(
      iree_make_string_view(lib_dir->data(), lib_dir->size()),
      iree_make_cstring_view("libIREECompiler.so"), iree_allocator_system(),
      &path);
  if (!iree_status_is_ok(status)) return {};

  std::string joined_path(path);
  iree_allocator_free(iree_allocator_system(), path);
  return joined_path;
}

}  // namespace iree::pjrt
