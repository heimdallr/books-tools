#pragma once

#include <QIdentityProxyModel>

#include "fnd/NonCopyMovable.h"
#include "fnd/memory.h"

namespace HomeCompa::FliFaq {

class Model final : public QIdentityProxyModel
{
	NON_COPY_MOVABLE(Model)

public:
	explicit Model(QObject* parent = nullptr);
	~Model() override;

private:
	PropagateConstPtr<QAbstractItemModel> m_source;
};

}
