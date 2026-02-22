#include "ui_TextViewWidget.h"

#include "TextViewWidget.h"

#include <QAbstractItemModel>

using namespace HomeCompa::FliFaq;
using namespace HomeCompa;

class TextViewWidget::Impl
{
public:
	Impl(QWidget& self, std::shared_ptr<const QAbstractItemModel> model)
		: m_model { std::move(model) }
	{
		m_ui.setupUi(&self);
		connect(m_model.get(), &QAbstractItemModel::dataChanged, [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
			if (roles.contains(m_role))
				UpdateText();
		});
	}

	void SetRole(const int role) noexcept
	{
		m_role = role;
	}

	void SetCurrentIndex(const QModelIndex& index)
	{
		m_currentIndex = index;
		UpdateText();
	}

private:
	void UpdateText() const
	{
		if (m_currentIndex.isValid())
			m_ui.textEdit->setHtml(m_model->data(m_currentIndex, m_role).toString());
	}

private:
	std::shared_ptr<const QAbstractItemModel> m_model;

	int                   m_role { -1 };
	QPersistentModelIndex m_currentIndex;

	Ui::TextViewWidget m_ui {};
};

TextViewWidget::TextViewWidget(std::shared_ptr<const QAbstractItemModel> model, QWidget* parent)
	: QWidget(parent)
	, m_impl(*this, std::move(model))
{
}

TextViewWidget::~TextViewWidget() = default;

void TextViewWidget::SetRole(const int role) noexcept
{
	m_impl->SetRole(role);
}

void TextViewWidget::SetCurrentIndex(const QModelIndex& index)
{
	m_impl->SetCurrentIndex(index);
}
