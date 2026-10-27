#pragma once
#include <QObject>
#include <QSettings>
#include <QString>

namespace Kites
{

enum class BorderStyle
{
    None,
    Subtle,
    Full
};

/**
 * @brief AppSettings persists user-facing UI preferences (theme, font, panel
 * styling) across application restarts using QSettings.
 */
class AppSettings : public QObject
{
    Q_OBJECT
  public:
    static AppSettings &getInstance();

    QString themeId() const;
    void setThemeId(const QString &id);

    // empty string = editor follows the global theme
    QString editorThemeId() const;
    void setEditorThemeId(const QString &id);

    QString editorFontFamily() const;
    void setEditorFontFamily(const QString &family);

    int editorFontSize() const;
    void setEditorFontSize(int size);

    BorderStyle compilerOutputBorderStyle() const;
    void setCompilerOutputBorderStyle(BorderStyle style);

    // Vector extension (see processor/vector/vector_config.h). VLEN takes effect at the next
    // processor reset; the other settings apply to the next instruction.
    int vectorVlen() const;
    void setVectorVlen(int vlen);
    bool vectorAgnosticOnes() const;
    void setVectorAgnosticOnes(bool ones);
    /// Profile and extensions as a comma-separated list, e.g. "v,zvbb".
    QString vectorExtensions() const;
    void setVectorExtensions(const QString &list);
    int vectorLanes() const;
    void setVectorLanes(int lanes);
    bool vectorChaining() const;
    void setVectorChaining(bool chaining);

    /// Copies the stored vector settings into rvv::config() and rvv::timingConfig(), falling
    /// back to the defaults for invalid values.
    void applyVectorSettings() const;

  signals:
    void compilerOutputBorderStyleChangedSignal(BorderStyle style);

  private:
    AppSettings();

    QSettings m_settings;
};

} // namespace Kites
