#pragma once

#include <QTableView>

namespace HomeCompa::FliFaq {

class TableView final : public QTableView
{
	Q_OBJECT

signals:
	void mouseDoubleClicked(QMouseEvent* event) const;

public:
	explicit TableView(QWidget* parent);

private: // QWidget
	void mouseDoubleClickEvent(QMouseEvent* event) override;
};

}
