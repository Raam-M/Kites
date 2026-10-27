#pragma once

#include "ui/common/kitestab.h"

#include <QTimer>

#include <cstdint>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QTableWidget;

namespace Kites
{

class ProcessorManager;

/**
 * @brief Element-wise view of the vector register file.
 *
 * Shows v0..v31 split into elements at the current SEW (or a chosen one) in hex, unsigned,
 * signed or floating-point form. Elements at or beyond vl are dimmed (tail), elements whose
 * v0 bit is 0 are shaded (masked off) when the mask overlay is on, register groups of the
 * current LMUL are banded, and elements changed by the last step are highlighted. Below the
 * table are the vector CSRs and the vector statistics and timing estimate.
 */
class VectorTab : public KitesTab
{
    Q_OBJECT

  public:
    VectorTab(QWidget *parent, ProcessorManager *manager);

  public slots:
    /// Coalesces refresh requests (the processor may clock much faster than the UI repaints).
    void scheduleRefresh();
    void refresh();

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    ProcessorManager *m_manager;
    QComboBox *m_sewCombo = nullptr;
    QComboBox *m_formatCombo = nullptr;
    QCheckBox *m_maskOverlay = nullptr;
    QCheckBox *m_groupBands = nullptr;
    QLabel *m_state = nullptr;
    QTableWidget *m_table = nullptr;
    QPlainTextEdit *m_stats = nullptr;
    QTimer m_refreshTimer;
    std::vector<uint8_t> m_previous; ///< register file at the previous refresh
};

} // namespace Kites
