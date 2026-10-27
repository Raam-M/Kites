#include "registermodel.h"
#include "common/register_names.h"
#include "processor/vector/vtype.h"
#include <QApplication>
#include <QBrush>
#include <QDebug>
#include <QPalette>

namespace Kites
{

namespace
{
struct VectorCsrRow
{
    const char *name;
    size_t address;
};

constexpr std::array<VectorCsrRow, RegisterModel::kVectorCsrCount> kVectorCsrRows = {{
    {"vl", csr_address::kVl},
    {"vtype", csr_address::kVtype},
    {"vstart", csr_address::kVstart},
    {"vxrm", csr_address::kVxrm},
    {"vxsat", csr_address::kVxsat},
    {"vcsr", csr_address::kVcsr},
    {"vlenb", csr_address::kVlenb},
}};

/// Vector register as one hex number, most significant byte first (element 0 rightmost).
QString vectorRegisterHex(const std::vector<uint8_t> &bytes)
{
    QString text = "0x";
    for (size_t b = bytes.size(); b-- > 0;)
        text += QString("%1").arg(bytes[b], 2, 16, QChar('0')).toUpper();
    return text;
}
} // namespace

RegisterModel::RegisterModel(QObject *parent, RegisterFile *regfile) : QAbstractTableModel(parent)
{
    m_currentRegisterFile = regfile;
    connect(m_currentRegisterFile, &RegisterFile::updateRegister, this,
            &RegisterModel::updateRegisterValue);
    connect(m_currentRegisterFile, &RegisterFile::updateFRegister, this,
            &RegisterModel::updateFRegisterValue);
    connect(m_currentRegisterFile, &RegisterFile::updateVRegister, this,
            &RegisterModel::updateVRegisterValue);
    connect(m_currentRegisterFile, &RegisterFile::updateCsr, this,
            &RegisterModel::updateCsrValue);
    connect(m_currentRegisterFile, &RegisterFile::registerResetSignal, this,
            &RegisterModel::registerResetSlot);
}

int RegisterModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return kRowCount;
}

int RegisterModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 3;
} // Register name, alias, value

void RegisterModel::setDisplayBase(Base base)
{
    beginResetModel();
    m_displayBase = base;
    endResetModel();
}

QVariant RegisterModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return QVariant{};

    if (orientation == Qt::Horizontal)
    {
        switch (section)
        {
        case 0:
            return QString("Register");
        case 1:
            return QString("Alias");
        case 2:
            return QString("Value");
        }
    }

    return QVariant{};
}

QVariant RegisterModel::data(const QModelIndex &index, int role) const
{
    if (!m_currentRegisterFile)
        return QVariant{};

    if (role == Qt::TextAlignmentRole)
    {
        return Qt::AlignCenter;
    }
    if (role == Qt::BackgroundRole)
    {
        if (static_cast<size_t>(index.row()) == m_highlightedRegisterIndex)
        {
            return QBrush(QColor("#b13232fd")); // Highlight color for the updated register
        }
    }
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
    {
        size_t row = static_cast<size_t>(index.row());
        switch (index.column())
        {
        case 0: // Register Name
            if (row < 32)
            {
                return QString("x%1").arg(row);
            }
            else if (row < kFirstVectorRow)
            {
                return QString("f%1").arg(row - 32);
            }
            else if (row < kFirstVectorCsrRow)
            {
                return QString(register_names::kVrNames[row - kFirstVectorRow]);
            }
            else
            {
                return QString(kVectorCsrRows[row - kFirstVectorCsrRow].name);
            }
        case 1: // Register Alias
        {
            if (row < 32)
                return QString(register_names::kGprAliases[row]);
            else if (row < 64)
                return QString(register_names::kFprAliases[row - 32]);
            else if (row >= kFirstVectorCsrRow &&
                     kVectorCsrRows[row - kFirstVectorCsrRow].address == csr_address::kVtype)
            {
                const uint64_t vtype = m_currentRegisterFile->ReadCsr(csr_address::kVtype);
                return (vtype & rvv::kVillBit) != 0 ? QString("vill")
                                                    : QString::fromStdString(rvv::vtypeToString(vtype));
            }
            else
                return QString("");
        }
        case 2: // Register Value
        {
            if (row >= kFirstVectorRow && row < kFirstVectorCsrRow)
            {
                // Vector registers are always shown in hex.
                return vectorRegisterHex(m_currentRegisterFile->ReadVr(row - kFirstVectorRow));
            }
            if (row >= kFirstVectorCsrRow)
            {
                const uint64_t value =
                    m_currentRegisterFile->ReadCsr(kVectorCsrRows[row - kFirstVectorCsrRow].address);
                switch (m_displayBase)
                {
                case Base::Binary:
                    return QString("0b%1").arg(QString::number(value, 2));
                case Base::Decimal:
                    return QString::number(value);
                case Base::Hexadecimal:
                    return QString("0x%1").arg(QString::number(value, 16).toUpper());
                }
            }
            if (row < 32)
            {
                uint64_t value = m_currentRegisterFile->ReadGpr(row);
                switch (m_displayBase)
                {
                case Base::Binary:
                    return QString("0b%1").arg(QString::number(value, 2));
                case Base::Decimal:
                    return QString::number(static_cast<int64_t>(value));
                case Base::Hexadecimal:
                    return QString("0x%1").arg(QString::number(value, 16).toUpper());
                }
            }
            else if (row < 64)
            {
                int fIndex = row - 32;

                // read raw bits of FP register (you must have this function)
                uint64_t raw = m_currentRegisterFile->ReadFpr(fIndex);

                switch (m_displayBase)
                {
                case Base::Binary:
                {
                    return QString("0b%1").arg(QString::number(raw, 2));
                }

                case Base::Decimal:
                {
                    uint32_t upper32 = static_cast<uint32_t>(raw >> 32);
                    if (upper32 == 0)
                    {
                        // Single-precision float (F-extension): value in lower 32 bits
                        uint32_t lower32 = static_cast<uint32_t>(raw & 0xFFFFFFFF);
                        float fval;
                        memcpy(&fval, &lower32, sizeof(float));
                        return QString::number(static_cast<double>(fval), 'g', 8);
                    }
                    else
                    {
                        // Double-precision (D-extension): full 64 bits
                        double dval;
                        memcpy(&dval, &raw, sizeof(double));
                        return QString::number(dval, 'g', 16);
                    }
                }

                case Base::Hexadecimal:
                {
                    return QString("0x%1").arg(QString::number(raw, 16).toUpper());
                }
                }
            }
            else
            {
                return QString("");
            } // TODO : add floating point register support
        }
        default:
            return QVariant{};
        }
    }
    return QVariant{};
}

void RegisterModel::changeRegisterFile(RegisterFile *regfile)
{
    beginResetModel();
    m_currentRegisterFile = regfile;
    connect(m_currentRegisterFile, &RegisterFile::updateRegister, this,
            &RegisterModel::updateRegisterValue);
    connect(m_currentRegisterFile, &RegisterFile::updateFRegister, this,
            &RegisterModel::updateFRegisterValue);
    connect(m_currentRegisterFile, &RegisterFile::updateVRegister, this,
            &RegisterModel::updateVRegisterValue);
    connect(m_currentRegisterFile, &RegisterFile::updateCsr, this,
            &RegisterModel::updateCsrValue);
    // maybe we can directly connect it to
    connect(m_currentRegisterFile, &RegisterFile::registerResetSignal, this,
            &RegisterModel::registerResetSlot);
    endResetModel();
}

void RegisterModel::registerResetSlot()
{
    beginResetModel();
    endResetModel();
}

void RegisterModel::updateRegisterValue(size_t regIndex, uint64_t value)
{
    //QModelIndex index = this->index(static_cast<int>(regIndex), 2);
    m_highlightedRegisterIndex = regIndex; // Update highlighted register index
    // emit dataChanged(index,index);
    beginResetModel();
    endResetModel();
}

void RegisterModel::updateVRegisterValue(size_t regIndex)
{
    m_highlightedRegisterIndex = regIndex + kFirstVectorRow;
    beginResetModel();
    endResetModel();
}

void RegisterModel::updateCsrValue(size_t address, uint64_t value)
{
    Q_UNUSED(value);
    for (int i = 0; i < kVectorCsrCount; ++i)
    {
        if (kVectorCsrRows[i].address == address)
        {
            beginResetModel();
            endResetModel();
            return;
        }
    }
}

void RegisterModel::updateFRegisterValue(size_t regIndex, uint64_t value)
{
    //QModelIndex index = this->index(static_cast<int>(regIndex + 32), 2);
    m_highlightedRegisterIndex = regIndex + 32; // Update highlighted register index
    // emit dataChanged(index,index);
    beginResetModel();
    endResetModel();
}
} // namespace Kites
