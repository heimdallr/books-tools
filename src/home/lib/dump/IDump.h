#pragma once

#include "fnd/EnumBitmask.h"
#include "fnd/algorithm.h"

namespace HomeCompa::DB
{

class IDatabase;
class IQuery;

}

namespace HomeCompa::FliLib
{

class IDump // NOLINT(cppcoreguidelines-special-member-functions)
{
public:
	struct DictionaryTableDescription
	{
		const char*              table;
		const char*              id;
		std::vector<const char*> names;
	};

	enum class AdditionalType
	{
		None       = 0,
		AuthorInfo = 1 << 0,
	};

public:
	virtual ~IDump() = default;

	virtual DB::IDatabase& SetDatabase(std::unique_ptr<DB::IDatabase>) noexcept = 0;

	virtual const QString& GetName() const noexcept = 0;

	virtual void CreateInpData(const std::function<void(const DB::IQuery&)>& functor) const = 0;
	virtual void CreateTables(const std::function<void(std::string_view)>& functor) const   = 0;
	virtual void CreateIndices(const std::function<void(std::string_view)>& functor) const  = 0;

	virtual void CreateAdditional(const std::filesystem::path& sqlDir, const std::filesystem::path& dstDir, AdditionalType additionalType) const = 0;

	virtual const DictionaryTableDescription& GetAuthorTable() const noexcept = 0;
	virtual const DictionaryTableDescription& GetSeriesTable() const noexcept = 0;

	virtual std::vector<std::pair<int, int>> GetReviewMonths() const                                                                                          = 0;
	virtual void                             Review(int year, int month, const std::function<void(const QString&, QString, QString, QString)>& functor) const = 0;
};

} // namespace HomeCompa::FliLib

ENABLE_BITMASK_OPERATORS(HomeCompa::FliLib::IDump::AdditionalType);
