#pragma once

#include <QIdentityProxyModel>

#include "fnd/NonCopyMovable.h"
#include "fnd/memory.h"

#include "util/ISettings.h"

namespace HomeCompa::FliFaq
{

class Model final : public QIdentityProxyModel
{
	NON_COPY_MOVABLE(Model)

public:
	explicit Model(const std::shared_ptr<const ISettings>& settings, QObject* parent = nullptr);
	~Model() override;

private:
	PropagateConstPtr<QAbstractItemModel> m_source;
};

}
