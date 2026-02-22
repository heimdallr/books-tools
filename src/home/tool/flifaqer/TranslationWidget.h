#pragma once

#include <QWidget>

#include "fnd/NonCopyMovable.h"
#include "fnd/memory.h"

#include "util/ISettings.h"

class QAbstractItemModel;

namespace HomeCompa::FliFaq
{

class TranslationWidget final : public QWidget
{
	Q_OBJECT
	NON_COPY_MOVABLE(TranslationWidget)

public:
	enum class Mode
	{
		Template,
		Reference,
		Translation,
	};

signals:
	void LanguageChanged() const;

public:
	TranslationWidget(std::shared_ptr<ISettings> settings, std::shared_ptr<QAbstractItemModel> model, QWidget* parent = nullptr);
	~TranslationWidget() override;

public:
	void SetMode(Mode mode);
	void AddLanguage(const QString& language) const;
	void SetLanguage(const QString& language) const;
	void SetCurrentIndex(const QModelIndex& index);

private:
	class Impl;
	PropagateConstPtr<Impl> m_impl;
};

} // namespace HomeCompa::FliFaq
