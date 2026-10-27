#include "app_settings.h"

#include "processor/vector/vector_config.h"
#include "processor/vector/vector_profiler.h"

namespace Kites
{

namespace
{
constexpr const char *kThemeIdKey = "theme/id";
constexpr const char *kEditorThemeIdKey = "theme/editorId";
constexpr const char *kEditorFontFamilyKey = "editor/fontFamily";
constexpr const char *kEditorFontSizeKey = "editor/fontSize";
constexpr const char *kCompilerOutputBorderStyleKey = "compilerOutput/borderStyle";
constexpr const char *kVectorVlenKey = "vector/vlen";
constexpr const char *kVectorAgnosticOnesKey = "vector/agnosticOnes";
constexpr const char *kVectorExtensionsKey = "vector/extensions";
constexpr const char *kVectorLanesKey = "vector/lanes";
constexpr const char *kVectorChainingKey = "vector/chaining";

constexpr int kDefaultEditorFontSize = 11;
} // namespace

AppSettings::AppSettings() : m_settings("Kites", "Kites")
{
}

AppSettings &AppSettings::getInstance()
{
    static AppSettings instance;
    return instance;
}

QString AppSettings::themeId() const
{
    return m_settings.value(kThemeIdKey).toString();
}

void AppSettings::setThemeId(const QString &id)
{
    m_settings.setValue(kThemeIdKey, id);
}

QString AppSettings::editorThemeId() const
{
    return m_settings.value(kEditorThemeIdKey).toString();
}

void AppSettings::setEditorThemeId(const QString &id)
{
    m_settings.setValue(kEditorThemeIdKey, id);
}

QString AppSettings::editorFontFamily() const
{
    return m_settings.value(kEditorFontFamilyKey).toString();
}

void AppSettings::setEditorFontFamily(const QString &family)
{
    m_settings.setValue(kEditorFontFamilyKey, family);
}

int AppSettings::editorFontSize() const
{
    return m_settings.value(kEditorFontSizeKey, kDefaultEditorFontSize).toInt();
}

void AppSettings::setEditorFontSize(int size)
{
    m_settings.setValue(kEditorFontSizeKey, size);
}

BorderStyle AppSettings::compilerOutputBorderStyle() const
{
    return static_cast<BorderStyle>(
        m_settings.value(kCompilerOutputBorderStyleKey, static_cast<int>(BorderStyle::Subtle)).toInt());
}

void AppSettings::setCompilerOutputBorderStyle(BorderStyle style)
{
    m_settings.setValue(kCompilerOutputBorderStyleKey, static_cast<int>(style));
    emit compilerOutputBorderStyleChangedSignal(style);
}

int AppSettings::vectorVlen() const
{
    return m_settings.value(kVectorVlenKey, static_cast<int>(rvv::kDefaultVlen)).toInt();
}

void AppSettings::setVectorVlen(int vlen)
{
    m_settings.setValue(kVectorVlenKey, vlen);
}

bool AppSettings::vectorAgnosticOnes() const
{
    return m_settings.value(kVectorAgnosticOnesKey, false).toBool();
}

void AppSettings::setVectorAgnosticOnes(bool ones)
{
    m_settings.setValue(kVectorAgnosticOnesKey, ones);
}

QString AppSettings::vectorExtensions() const
{
    return m_settings.value(kVectorExtensionsKey, "v").toString();
}

void AppSettings::setVectorExtensions(const QString &list)
{
    m_settings.setValue(kVectorExtensionsKey, list);
}

int AppSettings::vectorLanes() const
{
    return m_settings.value(kVectorLanesKey, static_cast<int>(rvv::TimingConfig{}.lanes)).toInt();
}

void AppSettings::setVectorLanes(int lanes)
{
    m_settings.setValue(kVectorLanesKey, lanes);
}

bool AppSettings::vectorChaining() const
{
    return m_settings.value(kVectorChainingKey, rvv::TimingConfig{}.chaining).toBool();
}

void AppSettings::setVectorChaining(bool chaining)
{
    m_settings.setValue(kVectorChainingKey, chaining);
}

void AppSettings::applyVectorSettings() const
{
    rvv::VectorConfig &cfg = rvv::config();
    const auto vlen = static_cast<unsigned>(vectorVlen());
    cfg.vlen = rvv::isSupportedVlen(vlen) ? vlen : rvv::kDefaultVlen;
    cfg.agnosticFillOnes = vectorAgnosticOnes();
    if (!rvv::applyExtensionList(cfg, vectorExtensions().toStdString()).empty())
        rvv::applyExtensionList(cfg, "v");

    rvv::TimingConfig &timing = rvv::timingConfig();
    const auto lanes = static_cast<unsigned>(vectorLanes());
    timing.lanes = rvv::isSupportedLaneCount(lanes) ? lanes : rvv::TimingConfig{}.lanes;
    timing.chaining = vectorChaining();
}

} // namespace Kites
