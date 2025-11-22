#pragma once

#include <memory>

namespace HomeCompa::FliParser
{

struct Settings;
class IDatabase;

namespace Database
{
std::unique_ptr<IDatabase> Create(const Settings& settings);
}

}
