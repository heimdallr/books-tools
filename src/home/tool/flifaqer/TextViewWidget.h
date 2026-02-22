#pragma once

#include <QWidget>

#include "fnd/NonCopyMovable.h"
#include "fnd/memory.h"

class QAbstractItemModel;

namespace HomeCompa::FliFaq
{

class TextViewWidget final : public QWidget
{
	NON_COPY_MOVABLE(TextViewWidget)

public:
	explicit TextViewWidget(std::shared_ptr<const QAbstractItemModel> model, QWidget* parent = nullptr);
	~TextViewWidget() override;

public:
	void SetRole(int role) noexcept;
	void SetCurrentIndex(const QModelIndex& index);

private:
	class Impl;
	PropagateConstPtr<Impl> m_impl;
};

}
