#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

class QFrame;
class QGridLayout;
class QLabel;
class QTableWidget;
class QTableWidgetItem;
class QWidget;

namespace aegis::desktop::ui {

[[nodiscard]] QLabel* CreateValueLabel(QWidget* parent, bool word_wrap = false);

void AddValueRow(QGridLayout* layout, int row, int label_column, const QString& label_text, QLabel*& value_label,
                 QWidget* parent);

[[nodiscard]] QFrame* CreateMetricCard(const QString& title, QLabel*& value_label, QWidget* parent);

[[nodiscard]] QTableWidgetItem* MakeReadOnlyItem(const QString& text);

void ConfigureReadOnlyTable(QTableWidget* table, const QStringList& headers, int minimum_height);

[[nodiscard]] QString FormatLocalDateTime(qint64 unix_time_milliseconds);

} // namespace aegis::desktop::ui
