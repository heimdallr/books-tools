#pragma once

#include <QWidget>

#include "fnd/NonCopyMovable.h"
#include "fnd/memory.h"

#include "utilgui/ScrollBarController.h"

class QAbstractItemModel;

namespace HomeCompa::FliFaq
{

class TextViewWidget final : public QWidget
{
	NON_COPY_MOVABLE(TextViewWidget)

public:
	TextViewWidget(std::shared_ptr<const QAbstractItemModel> model, std::shared_ptr<Util::ScrollBarController> scrollBarController, QWidget* parent = nullptr);
	~TextViewWidget() override;

public:
	void SetRole(int role) noexcept;
	void SetCurrentIndex(const QModelIndex& index);

private:
	class Impl;
	PropagateConstPtr<Impl> m_impl;
};

}
