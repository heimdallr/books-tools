#pragma once

#include "fnd/algorithm.h"

#include "book.h"

namespace HomeCompa::DB
{
class IDatabase;
class IQuery;
}

namespace HomeCompa::FliParser
{

struct Settings;

class IDatabase // NOLINT(cppcoreguidelines-special-member-functions)
{
public:
	virtual ~IDatabase() = default;

	virtual DB::IDatabase& SetDatabase(std::unique_ptr<DB::IDatabase>) noexcept = 0;

	virtual void CreateInpData(const std::function<void(const DB::IQuery&)>& functor) const                  = 0;
	virtual void CreateTables(const std::function<void(std::string_view)>& functor) const                    = 0;
	virtual void CreateIndices(const std::function<void(std::string_view)>& functor) const                   = 0;
	virtual void Review(const std::function<void(const QString&, QString, QString, QString)>& functor) const = 0;
	virtual void CreateAdditional(const Settings& settings) const                                            = 0;
};

}
