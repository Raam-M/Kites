#include "vectortab.h"

#include "processor/processor_manager.h"
#include "processor/registers.h"
#include "processor/vector/vector_config.h"
#include "processor/vector/vector_format.h"
#include "processor/vector/vector_profiler.h"
#include "processor/vector/vtype.h"
#include "ui/theme/font_manager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

namespace Kites
{

VectorTab::VectorTab(QWidget *parent, ProcessorManager *manager) : KitesTab(parent), m_manager(manager)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);

    auto *controls = new QHBoxLayout();
    m_sewCombo = new QComboBox(this);
    m_sewCombo->addItem("SEW from vtype", 0);
    for (unsigned sew : {8U, 16U, 32U, 64U})
        m_sewCombo->addItem(QString("e%1").arg(sew), sew);
    m_formatCombo = new QComboBox(this);
    m_formatCombo->addItem("Hex", static_cast<int>(rvv::ElementFormat::Hex));
    m_formatCombo->addItem("Unsigned", static_cast<int>(rvv::ElementFormat::Unsigned));
    m_formatCombo->addItem("Signed", static_cast<int>(rvv::ElementFormat::Signed));
    m_formatCombo->addItem("Float", static_cast<int>(rvv::ElementFormat::Float));
    m_maskOverlay = new QCheckBox("Shade masked-off elements (v0)", this);
    m_groupBands = new QCheckBox("Band LMUL groups", this);
    m_groupBands->setChecked(true);
    controls->addWidget(new QLabel("Elements:", this));
    controls->addWidget(m_sewCombo);
    controls->addWidget(m_formatCombo);
    controls->addSpacing(12);
    controls->addWidget(m_maskOverlay);
    controls->addWidget(m_groupBands);
    controls->addStretch(1);
    layout->addLayout(controls);

    m_state = new QLabel(this);
    m_state->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_state);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    m_table = new QTableWidget(this);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->verticalHeader()->setDefaultSectionSize(22);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_stats = new QPlainTextEdit(this);
    m_stats->setReadOnly(true);
    m_stats->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_table->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    splitter->addWidget(m_table);
    splitter->addWidget(m_stats);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(50);
    connect(&m_refreshTimer, &QTimer::timeout, this, &VectorTab::refresh);
    for (QComboBox *combo : {m_sewCombo, m_formatCombo})
        connect(combo, &QComboBox::currentIndexChanged, this, &VectorTab::refresh);
    for (QCheckBox *box : {m_maskOverlay, m_groupBands})
        connect(box, &QCheckBox::toggled, this, &VectorTab::refresh);

    // The processor clocks on its own thread: hop to the GUI thread without passing arguments.
    connect(m_manager, &ProcessorManager::processorClockedSignal, this,
            [this](const ProcessorState &)
            { QMetaObject::invokeMethod(this, &VectorTab::scheduleRefresh, Qt::QueuedConnection); },
            Qt::DirectConnection);
}

void VectorTab::showEvent(QShowEvent *event)
{
    KitesTab::showEvent(event);
    refresh();
}

void VectorTab::scheduleRefresh()
{
    if (isVisible() && !m_refreshTimer.isActive())
        m_refreshTimer.start();
}

void VectorTab::refresh()
{
    RegisterFile *registers = m_manager->getRegisterFile();
    if (registers == nullptr)
        return;

    const unsigned vlenb = static_cast<unsigned>(registers->GetVlenb());
    const std::vector<uint8_t> current = registers->GetVrBytes();
    if (m_previous.size() != current.size())
        m_previous = current;

    const uint64_t vtypeRaw = registers->ReadCsr(csr_address::kVtype);
    const rvv::VType vtype{vtypeRaw};
    const uint64_t vl = registers->ReadCsr(csr_address::kVl);
    const unsigned chosen = m_sewCombo->currentData().toUInt();
    const unsigned sew = chosen != 0 ? chosen : (vtype.vill() ? 8U : vtype.sew());
    const int lmulLog2 = vtype.vill() ? 0 : vtype.lmulLog2();
    const unsigned groupSize = lmulLog2 > 0 ? (1U << lmulLog2) : 1U;
    const auto format = static_cast<rvv::ElementFormat>(m_formatCombo->currentData().toInt());
    const unsigned perRegister = vlenb * 8 / sew;

    m_state->setText(QString("VLEN %1   vl %2   vtype %3   vstart %4   vxrm %5   vxsat %6   "
                             "(%7)")
                         .arg(vlenb * 8)
                         .arg(vl)
                         .arg(vtype.vill() ? QString("vill (no vset* yet, or illegal)")
                                           : QString::fromStdString(rvv::vtypeToString(vtypeRaw)))
                         .arg(registers->ReadCsr(csr_address::kVstart))
                         .arg(registers->ReadCsr(csr_address::kVxrm))
                         .arg(registers->ReadCsr(csr_address::kVxsat))
                         .arg(QString::fromStdString(rvv::config().describe())));

    m_table->setUpdatesEnabled(false);
    m_table->setRowCount(32);
    m_table->setColumnCount(static_cast<int>(perRegister));
    QStringList columns;
    for (unsigned i = 0; i < perRegister; ++i)
        columns << QString::number(i);
    m_table->setHorizontalHeaderLabels(columns);

    const QPalette pal = palette();
    QColor changed = pal.color(QPalette::Highlight);
    changed.setAlpha(110);
    QColor masked = pal.color(QPalette::Mid);
    masked.setAlpha(90);
    QColor band = pal.color(QPalette::AlternateBase);
    QColor tailText = pal.color(QPalette::Text);
    tailText.setAlpha(110);

    auto maskBit = [&](uint64_t index)
    {
        const uint64_t byte = index / 8;
        return byte < vlenb && ((current[byte] >> (index % 8)) & 1U) != 0;
    };

    for (unsigned r = 0; r < 32; ++r)
    {
        const unsigned groupStart = r - (r % groupSize);
        auto *header = new QTableWidgetItem(groupSize > 1 && r == groupStart
                                                ? QString("v%1 (v%1-v%2)").arg(r).arg(r + groupSize - 1)
                                                : QString("v%1").arg(r));
        m_table->setVerticalHeaderItem(static_cast<int>(r), header);
        const std::vector<uint8_t> bytes(current.begin() + r * vlenb, current.begin() + (r + 1) * vlenb);
        const std::vector<uint8_t> before(m_previous.begin() + r * vlenb,
                                          m_previous.begin() + (r + 1) * vlenb);
        const auto values = rvv::splitElements(bytes, sew);
        const auto old = rvv::splitElements(before, sew);
        const bool banded = m_groupBands->isChecked() && groupSize > 1 && ((r / groupSize) % 2 == 1);
        for (unsigned i = 0; i < perRegister; ++i)
        {
            auto *item = new QTableWidgetItem(
                QString::fromStdString(rvv::formatElement(values[i], sew, format)));
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            // Element index within the register group, for vl / mask decisions.
            const uint64_t index = static_cast<uint64_t>(r - groupStart) * perRegister + i;
            const bool tail = index >= vl;
            if (tail)
                item->setForeground(tailText);
            if (values[i] != old[i])
                item->setBackground(changed);
            else if (m_maskOverlay->isChecked() && !tail && !maskBit(index))
                item->setBackground(masked);
            else if (banded)
                item->setBackground(band);
            item->setToolTip(QString("v%1[%2]  e%3\nhex %4\nunsigned %5\nsigned %6%7")
                                 .arg(r)
                                 .arg(i)
                                 .arg(sew)
                                 .arg(QString::fromStdString(
                                     rvv::formatElement(values[i], sew, rvv::ElementFormat::Hex)))
                                 .arg(QString::fromStdString(rvv::formatElement(
                                     values[i], sew, rvv::ElementFormat::Unsigned)))
                                 .arg(QString::fromStdString(rvv::formatElement(
                                     values[i], sew, rvv::ElementFormat::Signed)))
                                 .arg(tail ? "\ntail element (index >= vl)" : ""));
            m_table->setItem(static_cast<int>(r), static_cast<int>(i), item);
        }
    }
    m_table->setUpdatesEnabled(true);
    m_previous = current;

    m_stats->setPlainText(QString::fromStdString(m_manager->getVectorProfiler()->report()));
}

} // namespace Kites
