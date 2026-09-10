// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/OperationPlan.h>

#include <QDialog>
#include <QTreeWidget>

namespace cullfinch::ui {

/// Shows exactly what a file operation would touch, before anything moves.
///
/// The primary action is Move to Trash. Pressing Delete during a comparison
/// never reaches this screen: marking and executing are deliberately separate,
/// so a marking-only milestone stays useful on its own.
class ReviewDialog : public QDialog {
    Q_OBJECT

public:
    ReviewDialog(const domain::PlanningResult& planning, QWidget* parent = nullptr);

    [[nodiscard]] bool executionRequested() const { return executionRequested_; }
    [[nodiscard]] const domain::OperationPlan& plan() const { return planning_.plan; }

private:
    void populate();

    domain::PlanningResult planning_;
    QTreeWidget* tree_ = nullptr;
    bool executionRequested_ = false;
};

} // namespace cullfinch::ui
