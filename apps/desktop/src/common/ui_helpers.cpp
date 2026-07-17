#include "desktop/common/ui_helpers.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimeZone>
#include <QVBoxLayout>

namespace aegis::desktop::ui {

QLabel* CreateValueLabel(QWidget* parent, const bool word_wrap) {
    auto* label = new QLabel("-", parent);

    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(word_wrap);

    return label;
}

void AddValueRow(QGridLayout* layout, const int row, const int label_column, const QString& label_text,
                 QLabel*& value_label, QWidget* parent) {
    layout->addWidget(new QLabel(label_text, parent), row, label_column);
    value_label = CreateValueLabel(parent, true);
    layout->addWidget(value_label, row, label_column + 1);
}

QFrame* CreateMetricCard(const QString& title, QLabel*& value_label, QWidget* parent) {
    auto* card = new QFrame(parent);

    card->setFrameShape(QFrame::StyledPanel);
    card->setFrameShadow(QFrame::Raised);

    auto* layout = new QVBoxLayout(card);
    auto* title_label = new QLabel(title, card);

    value_label = CreateValueLabel(card);

    QFont value_font = value_label->font();
    value_font.setBold(true);
    value_font.setPointSize(value_font.pointSize() + 2);

    value_label->setFont(value_font);

    layout->addWidget(title_label);
    layout->addWidget(value_label);
    layout->addStretch();

    return card;
}

QTableWidgetItem* MakeReadOnlyItem(const QString& text) {
    auto* item = new QTableWidgetItem(text);

    item->setFlags(item->flags() & ~Qt::ItemIsEditable);

    return item;
}

void ConfigureReadOnlyTable(QTableWidget* table, const QStringList& headers, const int minimum_height) {
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->setMinimumHeight(minimum_height);
}

QString FormatLocalDateTime(const qint64 unix_time_milliseconds) {
    if (unix_time_milliseconds <= 0) {
        return "-";
    }

    const QDateTime utc_time = QDateTime::fromMSecsSinceEpoch(unix_time_milliseconds, QTimeZone::utc());

    return utc_time.toLocalTime().toString("yyyy-MM-dd HH:mm:ss");
}

} // namespace aegis::desktop::ui
