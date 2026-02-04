#pragma once

#include <filesystem>
#include <memory>

#include <QString>

#include "export/lib.h"

namespace HomeCompa
{

namespace FliLib
{

class IDump;

namespace Dump
{

LIB_EXPORT std::unique_ptr<IDump> Create(const std::filesystem::path& sqlDir, const std::filesystem::path& dbPath, const QString& sourceLib = {}, const std::filesystem::path& replacementPath = {});
LIB_EXPORT QStringList            GetAvailableLibraries();

}

}

}
