/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the tools applications of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL-EXCEPT$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "cppwriteinitialization.h"
#include "driver.h"
#include "ui4.h"
#include "utils.h"
#include "uic.h"
#include "databaseinfo.h"

#include <language.h>

#include <qtextstream.h>
#include <qversionnumber.h>
#include <qdebug.h>

#include <algorithm>

#include <ctype.h>

QT_BEGIN_NAMESPACE

namespace {
    // figure out the toolbar area of a DOM attrib list.
    // By legacy, it is stored as an integer. As of 4.3.0, it is the enumeration value.
    QString toolBarAreaStringFromDOMAttributes(const CPP::WriteInitialization::DomPropertyMap &attributes) {
        const DomProperty *pstyle = attributes.value(QLatin1String("toolBarArea"));
        QString result;
        if (!pstyle)
            return result;
        switch (pstyle->kind()) {
        case DomProperty::Number:
            result = QLatin1String(language::toolbarArea(pstyle->elementNumber()));
            break;
        case DomProperty::Enum:
            result = pstyle->elementEnum();
            break;
        default:
            break;
        }
        if (!result.startsWith(QLatin1String("Qt::")))
            result.prepend(QLatin1String("Qt::"));
        return result + QLatin1String(", ");
    }

    // Write a statement to create a spacer item.
    void writeSpacerItem(const DomSpacer *node, QTextStream &output) {
        const QHash<QString, DomProperty *> properties = propertyMap(node->elementProperty());
                output << language::operatorNew << "QSpacerItem(";

        int w = 0;
        int h = 0;
        if (const DomProperty *sh = properties.value(QLatin1String("sizeHint"))) {
            if (const DomSize *sizeHint = sh->elementSize()) {
                w = sizeHint->elementWidth();
                h = sizeHint->elementHeight();
            }
        }
        output << w << ", " << h << ", ";

        // size type
        QString sizeType;
        if (const DomProperty *st = properties.value(QLatin1String("sizeType"))) {
            const QString value = st->elementEnum();
            if (value.startsWith(QLatin1String("QSizePolicy::")))
                sizeType = value;
            else
                sizeType = QLatin1String("QSizePolicy::") + value;
        } else {
            sizeType = QStringLiteral("QSizePolicy::Expanding");
        }

        // orientation
        bool isVspacer = false;
        if (const DomProperty *o = properties.value(QLatin1String("orientation"))) {
            const QString orientation = o->elementEnum();
            if (orientation == QLatin1String("Qt::Vertical") || orientation == QLatin1String("Vertical"))
                isVspacer = true;
        }
        const QString horizType = isVspacer ? QLatin1String("QSizePolicy::Minimum") : sizeType;
        const QString vertType = isVspacer ? sizeType : QLatin1String("QSizePolicy::Minimum");
        output << language::enumValue(horizType) << ", " << language::enumValue(vertType) << ')';
    }


    // Helper for implementing comparison functions for integers.
    int compareInt(int i1, int i2) {
        if (i1 < i2) return -1;
        if (i1 > i2) return  1;
        return  0;
    }

    // Write object->setFoo(x);
    template <class Value>
        void writeSetter(const QString &indent, const QString &varName,const QString &setter, Value v, QTextStream &str) {
            str << indent << varName << language::derefPointer
                << setter << '(' << v << ')' << language::eol;
        }

    static inline bool iconHasStatePixmaps(const DomResourceIcon *i) {
        return i->hasElementNormalOff()   || i->hasElementNormalOn() ||
               i->hasElementDisabledOff() || i->hasElementDisabledOn() ||
               i->hasElementActiveOff()   || i->hasElementActiveOn() ||
               i->hasElementSelectedOff() || i->hasElementSelectedOn();
    }

    static inline bool isIconFormat44(const DomResourceIcon *i) {
        return iconHasStatePixmaps(i) || !i->attributeTheme().isEmpty();
    }

    // Check on properties. Filter out empty legacy pixmap/icon properties
    // as Designer pre 4.4 used to remove missing resource references.
    // This can no longer be handled by the code as we have 'setIcon(QIcon())' as well as 'QIcon icon'
    static bool checkProperty(const QString &fileName, const DomProperty *p) {
        switch (p->kind()) {
        case DomProperty::IconSet:
            if (const DomResourceIcon *dri = p->elementIconSet()) {
                if (!isIconFormat44(dri)) {
                    if (dri->text().isEmpty())  {
                        const QString msg = QString::fromLatin1("%1: Warning: An invalid icon property '%2' was encountered.")
                                            .arg(fileName, p->attributeName());
                        qWarning("%s", qPrintable(msg));
                        return false;
                    }
                }
            }
            break;
        case DomProperty::Pixmap:
            if (const DomResourcePixmap *drp = p->elementPixmap())
                if (drp->text().isEmpty()) {
                    const QString msg = QString::fromUtf8("%1: Warning: An invalid pixmap property '%2' was encountered.")
                                        .arg(fileName, p->attributeName());
                    qWarning("%s", qPrintable(msg));
                    return false;
                }
            break;
        default:
            break;
        }
        return  true;
    }
}

// QtGui
static inline QString accessibilityConfigKey() { return QStringLiteral("accessibility"); }
static inline QString shortcutConfigKey()      { return QStringLiteral("shortcut"); }
static inline QString whatsThisConfigKey()     { return QStringLiteral("whatsthis"); }
// QtWidgets
static inline QString statusTipConfigKey()     { return QStringLiteral("statustip"); }
static inline QString toolTipConfigKey()       { return QStringLiteral("tooltip"); }

namespace CPP {

FontHandle::FontHandle(const DomFont *domFont) :
      m_domFont(domFont)
{
}

int FontHandle::compare(const FontHandle &rhs) const
{
    const QString family    = m_domFont->hasElementFamily()     ?     m_domFont->elementFamily() : QString();
    const QString rhsFamily = rhs.m_domFont->hasElementFamily() ? rhs.m_domFont->elementFamily() : QString();

    if (const int frc = family.compare(rhsFamily))
        return frc;

    const int pointSize    = m_domFont->hasElementPointSize()     ?     m_domFont->elementPointSize() : -1;
    const int rhsPointSize = rhs.m_domFont->hasElementPointSize() ? rhs.m_domFont->elementPointSize() : -1;

    if (const int crc = compareInt(pointSize, rhsPointSize))
        return crc;

    const int bold    = m_domFont->hasElementBold()     ? (m_domFont->elementBold()     ? 1 : 0) : -1;
    const int rhsBold = rhs.m_domFont->hasElementBold() ? (rhs.m_domFont->elementBold() ? 1 : 0) : -1;
    if (const int crc = compareInt(bold, rhsBold))
        return crc;

    const int italic    = m_domFont->hasElementItalic()     ? (m_domFont->elementItalic()     ? 1 : 0) : -1;
    const int rhsItalic = rhs.m_domFont->hasElementItalic() ? (rhs.m_domFont->elementItalic() ? 1 : 0) : -1;
    if (const int crc = compareInt(italic, rhsItalic))
        return crc;

    const int underline    = m_domFont->hasElementUnderline()     ? (m_domFont->elementUnderline()     ? 1 : 0) : -1;
    const int rhsUnderline = rhs.m_domFont->hasElementUnderline() ? (rhs.m_domFont->elementUnderline() ? 1 : 0) : -1;
    if (const int crc = compareInt(underline, rhsUnderline))
        return crc;

    const int weight    = m_domFont->hasElementWeight()     ?     m_domFont->elementWeight() : -1;
    const int rhsWeight = rhs.m_domFont->hasElementWeight() ? rhs.m_domFont->elementWeight() : -1;
    if (const int crc = compareInt(weight, rhsWeight))
        return crc;

    const int strikeOut    = m_domFont->hasElementStrikeOut()     ? (m_domFont->elementStrikeOut()     ? 1 : 0) : -1;
    const int rhsStrikeOut = rhs.m_domFont->hasElementStrikeOut() ? (rhs.m_domFont->elementStrikeOut() ? 1 : 0) : -1;
    if (const int crc = compareInt(strikeOut, rhsStrikeOut))
        return crc;

    const int kerning    = m_domFont->hasElementKerning()     ? (m_domFont->elementKerning()     ? 1 : 0) : -1;
    const int rhsKerning = rhs.m_domFont->hasElementKerning() ? (rhs.m_domFont->elementKerning() ? 1 : 0) : -1;
    if (const int crc = compareInt(kerning, rhsKerning))
        return crc;

    const int antialiasing    = m_domFont->hasElementAntialiasing()     ? (m_domFont->elementAntialiasing()     ? 1 : 0) : -1;
    const int rhsAntialiasing = rhs.m_domFont->hasElementAntialiasing() ? (rhs.m_domFont->elementAntialiasing() ? 1 : 0) : -1;
    if (const int crc = compareInt(antialiasing, rhsAntialiasing))
        return crc;

    const QString styleStrategy    = m_domFont->hasElementStyleStrategy()     ?     m_domFont->elementStyleStrategy() : QString();
    const QString rhsStyleStrategy = rhs.m_domFont->hasElementStyleStrategy() ? rhs.m_domFont->elementStyleStrategy() : QString();

    if (const int src = styleStrategy.compare(rhsStyleStrategy))
        return src;

    return 0;
}

IconHandle::IconHandle(const DomResourceIcon *domIcon) :
      m_domIcon(domIcon)
{
}

int IconHandle::compare(const IconHandle &rhs) const
{
    if (const int comp = m_domIcon->attributeTheme().compare(rhs.m_domIcon->attributeTheme()))
        return comp;

    const QString normalOff    =     m_domIcon->hasElementNormalOff() ?     m_domIcon->elementNormalOff()->text() : QString();
    const QString rhsNormalOff = rhs.m_domIcon->hasElementNormalOff() ? rhs.m_domIcon->elementNormalOff()->text() : QString();
    if (const int comp = normalOff.compare(rhsNormalOff))
        return comp;

    const QString normalOn    =     m_domIcon->hasElementNormalOn() ?     m_domIcon->elementNormalOn()->text() : QString();
    const QString rhsNormalOn = rhs.m_domIcon->hasElementNormalOn() ? rhs.m_domIcon->elementNormalOn()->text() : QString();
    if (const int comp = normalOn.compare(rhsNormalOn))
        return comp;

    const QString disabledOff    =     m_domIcon->hasElementDisabledOff() ?     m_domIcon->elementDisabledOff()->text() : QString();
    const QString rhsDisabledOff = rhs.m_domIcon->hasElementDisabledOff() ? rhs.m_domIcon->elementDisabledOff()->text() : QString();
    if (const int comp = disabledOff.compare(rhsDisabledOff))
        return comp;

    const QString disabledOn    =     m_domIcon->hasElementDisabledOn() ?     m_domIcon->elementDisabledOn()->text() : QString();
    const QString rhsDisabledOn = rhs.m_domIcon->hasElementDisabledOn() ? rhs.m_domIcon->elementDisabledOn()->text() : QString();
    if (const int comp = disabledOn.compare(rhsDisabledOn))
        return comp;

    const QString activeOff    =     m_domIcon->hasElementActiveOff() ?     m_domIcon->elementActiveOff()->text() : QString();
    const QString rhsActiveOff = rhs.m_domIcon->hasElementActiveOff() ? rhs.m_domIcon->elementActiveOff()->text() : QString();
    if (const int comp = activeOff.compare(rhsActiveOff))
        return comp;

    const QString activeOn    =     m_domIcon->hasElementActiveOn() ?     m_domIcon->elementActiveOn()->text() : QString();
    const QString rhsActiveOn = rhs.m_domIcon->hasElementActiveOn() ? rhs.m_domIcon->elementActiveOn()->text() : QString();
    if (const int comp = activeOn.compare(rhsActiveOn))
        return comp;

    const QString selectedOff    =     m_domIcon->hasElementSelectedOff() ?     m_domIcon->elementSelectedOff()->text() : QString();
    const QString rhsSelectedOff = rhs.m_domIcon->hasElementSelectedOff() ? rhs.m_domIcon->elementSelectedOff()->text() : QString();
    if (const int comp = selectedOff.compare(rhsSelectedOff))
        return comp;

    const QString selectedOn    =     m_domIcon->hasElementSelectedOn() ?     m_domIcon->elementSelectedOn()->text() : QString();
    const QString rhsSelectedOn = rhs.m_domIcon->hasElementSelectedOn() ? rhs.m_domIcon->elementSelectedOn()->text() : QString();
    if (const int comp = selectedOn.compare(rhsSelectedOn))
        return comp;
    // Pre 4.4 Legacy
    if (const int comp = m_domIcon->text().compare(rhs.m_domIcon->text()))
        return comp;

    return 0;
}

SizePolicyHandle::SizePolicyHandle(const DomSizePolicy *domSizePolicy) :
    m_domSizePolicy(domSizePolicy)
{
}

int SizePolicyHandle::compare(const SizePolicyHandle &rhs) const
{

    const int hSizeType    = m_domSizePolicy->hasElementHSizeType()     ? m_domSizePolicy->elementHSizeType()     : -1;
    const int rhsHSizeType = rhs.m_domSizePolicy->hasElementHSizeType() ? rhs.m_domSizePolicy->elementHSizeType() : -1;
    if (const int crc = compareInt(hSizeType, rhsHSizeType))
        return crc;

    const int vSizeType    = m_domSizePolicy->hasElementVSizeType()     ? m_domSizePolicy->elementVSizeType()     : -1;
    const int rhsVSizeType = rhs.m_domSizePolicy->hasElementVSizeType() ? rhs.m_domSizePolicy->elementVSizeType() : -1;
    if (const int crc = compareInt(vSizeType, rhsVSizeType))
        return crc;

    const int hStretch    =  m_domSizePolicy->hasElementHorStretch()     ? m_domSizePolicy->elementHorStretch()     : -1;
    const int rhsHStretch =  rhs.m_domSizePolicy->hasElementHorStretch() ? rhs.m_domSizePolicy->elementHorStretch() : -1;
    if (const int crc = compareInt(hStretch, rhsHStretch))
        return crc;

    const int vStretch    =  m_domSizePolicy->hasElementVerStretch()     ? m_domSizePolicy->elementVerStretch()     : -1;
    const int rhsVStretch =  rhs.m_domSizePolicy->hasElementVerStretch() ? rhs.m_domSizePolicy->elementVerStretch() : -1;
    if (const int crc = compareInt(vStretch, rhsVStretch))
        return crc;

    const QString attributeHSizeType    = m_domSizePolicy->hasAttributeHSizeType()     ? m_domSizePolicy->attributeHSizeType()     : QString();
    const QString rhsAttributeHSizeType = rhs.m_domSizePolicy->hasAttributeHSizeType() ? rhs.m_domSizePolicy->attributeHSizeType() : QString();

    if (const int hrc = attributeHSizeType.compare(rhsAttributeHSizeType))
        return hrc;

    const QString attributeVSizeType    = m_domSizePolicy->hasAttributeVSizeType()     ? m_domSizePolicy->attributeVSizeType()     : QString();
    const QString rhsAttributeVSizeType = rhs.m_domSizePolicy->hasAttributeVSizeType() ? rhs.m_domSizePolicy->attributeVSizeType() : QString();

    return attributeVSizeType.compare(rhsAttributeVSizeType);
}

// ---  WriteInitialization: LayoutDefaultHandler

WriteInitialization::LayoutDefaultHandler::LayoutDefaultHandler()
{
    std::fill_n(m_state, int(NumProperties), 0u);
    std::fill_n(m_defaultValues, int(NumProperties), 0);
}



void WriteInitialization::LayoutDefaultHandler::acceptLayoutDefault(DomLayoutDefault *node)
{
    if (!node)
        return;
    if (node->hasAttributeMargin()) {
        m_state[Margin] |= HasDefaultValue;
        m_defaultValues[Margin] = node->attributeMargin();
    }
    if (node->hasAttributeSpacing()) {
        m_state[Spacing] |= HasDefaultValue;
        m_defaultValues[Spacing]  = node->attributeSpacing();
    }
}

void WriteInitialization::LayoutDefaultHandler::acceptLayoutFunction(DomLayoutFunction *node)
{
    if (!node)
        return;
    if (node->hasAttributeMargin()) {
        m_state[Margin]     |= HasDefaultFunction;
        m_functions[Margin] =  node->attributeMargin();
        m_functions[Margin] += QLatin1String("()");
    }
    if (node->hasAttributeSpacing()) {
        m_state[Spacing]     |= HasDefaultFunction;
        m_functions[Spacing] =  node->attributeSpacing();
        m_functions[Spacing] += QLatin1String("()");
    }
}

static inline void writeContentsMargins(const QString &indent, const QString &objectName, int value, QTextStream &str)
{
     QString contentsMargins;
     QTextStream(&contentsMargins) << value << ", " << value << ", " << value << ", " << value;
     writeSetter(indent, objectName, QLatin1String("setContentsMargins"), contentsMargins, str);
 }

void WriteInitialization::LayoutDefaultHandler::writeProperty(int p, const QString &indent, const QString &objectName,
                                                              const DomPropertyMap &properties, const QString &propertyName, const QString &setter,
                                                              int defaultStyleValue, bool suppressDefault, QTextStream &str) const
{
    // User value
    if (const DomProperty *prop = properties.value(propertyName)) {
        const int value = prop->elementNumber();
        // Emulate the pre 4.3 behaviour: The value form default value was only used to determine
        // the default value, layout properties were always written
        const bool useLayoutFunctionPre43 = !suppressDefault && (m_state[p] == (HasDefaultFunction|HasDefaultValue)) && value == m_defaultValues[p];
        if (!useLayoutFunctionPre43) {
            bool ifndefMac = (!(m_state[p] & (HasDefaultFunction|HasDefaultValue))
                             && value == defaultStyleValue);
            if (ifndefMac)
                str << "#ifndef Q_OS_MAC\n";
            if (p == Margin) { // Use setContentsMargins for numeric values
                writeContentsMargins(indent, objectName, value, str);
            } else {
                writeSetter(indent, objectName, setter, value, str);
            }
            if (ifndefMac)
                str << "#endif\n";
            return;
        }
    }
    if (suppressDefault)
        return;
    // get default.
    if (m_state[p] & HasDefaultFunction) {
        // Do not use setContentsMargins to avoid repetitive evaluations.
        writeSetter(indent, objectName, setter, m_functions[p], str);
        return;
    }
    if (m_state[p] & HasDefaultValue) {
        if (p == Margin) { // Use setContentsMargins for numeric values
            writeContentsMargins(indent, objectName, m_defaultValues[p], str);
        } else {
            writeSetter(indent, objectName, setter, m_defaultValues[p], str);
        }
    }
    return;
}


void WriteInitialization::LayoutDefaultHandler::writeProperties(const QString &indent, const QString &varName,
                                                                const DomPropertyMap &properties, int marginType,
                                                                bool suppressMarginDefault,
                                                                QTextStream &str) const {
    // Write out properties and ignore the ones found in
    // subsequent writing of the property list.
    int defaultSpacing = marginType == WriteInitialization::Use43UiFile ? -1 : 6;
    writeProperty(Spacing, indent, varName, properties, QLatin1String("spacing"), QLatin1String("setSpacing"),
                  defaultSpacing, false, str);
    // We use 9 as TopLevelMargin, since Designer seem to always use 9.
    static const int layoutmargins[4] = {-1, 9, 9, 0};
    writeProperty(Margin,  indent, varName, properties, QLatin1String("margin"),  QLatin1String("setMargin"),
                  layoutmargins[marginType], suppressMarginDefault, str);
}

template <class DomElement> // (DomString, DomStringList)
static bool needsTranslation(const DomElement *element)
{
    if (!element)
        return false;
    return !element->hasAttributeNotr() || !toBool(element->attributeNotr());
}

// ---  WriteInitialization
WriteInitialization::WriteInitialization(Uic *uic) :
      m_uic(uic),
      m_driver(uic->driver()), m_output(uic->output()), m_option(uic->option()),
      m_indent(m_option.indent + m_option.indent),
      m_dindent(m_indent + m_option.indent),
      m_delayedOut(&m_delayedInitialization, QIODevice::WriteOnly),
      m_refreshOut(&m_refreshInitialization, QIODevice::WriteOnly),
      m_actionOut(&m_delayedActionInitialization, QIODevice::WriteOnly)
{
}

void WriteInitialization::acceptUI(DomUI *node)
{
    m_actionGroupChain.push(nullptr);
    m_widgetChain.push(nullptr);
    m_layoutChain.push(nullptr);

    if (node->hasAttributeConnectslotsbyname())
        m_connectSlotsByName = node->attributeConnectslotsbyname();

    if (auto customSlots = node->elementSlots()) {
        m_customSlots = customSlots->elementSlot();
        m_customSignals = customSlots->elementSignal();
    }

    acceptLayoutDefault(node->elementLayoutDefault());
    acceptLayoutFunction(node->elementLayoutFunction());

    if (node->elementCustomWidgets())
        TreeWalker::acceptCustomWidgets(node->elementCustomWidgets());

    if (m_option.generateImplemetation)
        m_output << "#include <" << m_driver->headerFileName() << ">\n\n";

    m_stdsetdef = true;
    if (node->hasAttributeStdSetDef())
        m_stdsetdef = node->attributeStdSetDef();

    const QString className = node->elementClass() + m_option.postfix;
    m_generatedClass = className;

    const QString varName = m_driver->findOrInsertWidget(node->elementWidget());
    m_mainFormVarName = varName;

    const QString widgetClassName = node->elementWidget()->attributeClass();

    const QString parameterType = widgetClassName + QLatin1String(" *");
    m_output << m_option.indent
             << language::startFunctionDefinition1("setupUi", parameterType, varName, m_option.indent);

    const QStringList connections = m_uic->databaseInfo()->connections();
    for (const auto &connection : connections) {
        if (connection == QLatin1String("(default)"))
            continue;

        const QString varConn = connection + QLatin1String("Connection");
        m_output << m_indent << varConn << " = QSqlDatabase::database("
            << language::charliteral(connection, m_dindent) << ")" << language::eol;
    }

    acceptWidget(node->elementWidget());

    if (!m_buddies.empty())
        m_output << language::openQtConfig(shortcutConfigKey());
    for (const Buddy &b : qAsConst(m_buddies)) {
        const QString buddyVarName = m_driver->widgetVariableName(b.buddyAttributeName);
        if (buddyVarName.isEmpty()) {
            fprintf(stderr, "%s: Warning: Buddy assignment: '%s' is not a valid widget.\n",
                    qPrintable(m_option.messagePrefix()),
                    qPrintable(b.buddyAttributeName));
            continue;
        }

        m_output << m_indent << b.labelVarName << language::derefPointer
            << "setBuddy(" << buddyVarName << ')' << language::eol;
    }
    if (!m_buddies.empty())
        m_output << language::closeQtConfig(shortcutConfigKey());

    if (node->elementTabStops())
        acceptTabStops(node->elementTabStops());

    if (!m_delayedActionInitialization.isEmpty())
        m_output << "\n" << m_delayedActionInitialization;

    m_output << "\n" << m_indent << language::self
        << "retranslateUi(" << varName << ')' << language::eol;

    if (node->elementConnections())
        acceptConnections(node->elementConnections());

    if (!m_delayedInitialization.isEmpty())
        m_output << "\n" << m_delayedInitialization << "\n";

    if (m_option.autoConnection && m_connectSlotsByName) {
        m_output << "\n" << m_indent << "QMetaObject" << language::qualifier
            << "connectSlotsByName(" << varName << ')' << language::eol;
    }

    m_output << m_option.indent << language::endFunctionDefinition("setupUi");

    if (!m_mainFormUsedInRetranslateUi) {
        if (language::language() == Language::Cpp) {
            // Mark varName as unused to avoid compiler warnings.
            m_refreshInitialization += m_indent;
            m_refreshInitialization += QLatin1String("(void)");
            m_refreshInitialization += varName ;
            m_refreshInitialization += language::eol;
        } else if (language::language() == Language::Python) {
            // output a 'pass' to have an empty function
            m_refreshInitialization += m_indent;
            m_refreshInitialization += QLatin1String("pass");
            m_refreshInitialization += language::eol;
        }
    }

    m_output << m_option.indent
           << language::startFunctionDefinition1("retranslateUi", parameterType, varName, m_option.indent)
           << m_refreshInitialization
           << m_option.indent << language::endFunctionDefinition("retranslateUi");

    m_layoutChain.pop();
    m_widgetChain.pop();
    m_actionGroupChain.pop();
}

void WriteInitialization::addWizardPage(const QString &pageVarName, const DomWidget *page, const QString &parentWidget)
{
    /* If the node has a (free-format) string "pageId" attribute (which could
     * an integer or an enumeration value), use setPage(), else addPage(). */
    QString id;
    const auto &attributes = page->elementAttribute();
    if (!attributes.empty()) {
        for (const DomProperty *p : attributes) {
            if (p->attributeName() == QLatin1String("pageId")) {
                if (const DomString *ds = p->elementString())
                    id = ds->text();
                break;
            }
        }
    }
    if (id.isEmpty()) {
        m_output << m_indent << parentWidget << language::derefPointer
            << "addPage(" << pageVarName << ')' << language::eol;
    } else {
        m_output << m_indent << parentWidget << language::derefPointer
            << "setPage(" << id << ", " << pageVarName << ')' << language::eol;
    }
}

void WriteInitialization::acceptWidget(DomWidget *node)
{
    m_layoutMarginType = m_widgetChain.count() == 1 ? TopLevelMargin : ChildMargin;
    const QString className = node->attributeClass();
    const QString varName = m_driver->findOrInsertWidget(node);

    QString parentWidget, parentClass;
    if (m_widgetChain.top()) {
        parentWidget = m_driver->findOrInsertWidget(m_widgetChain.top());
        parentClass = m_widgetChain.top()->attributeClass();
    }

    const QString savedParentWidget = parentWidget;

    if (m_uic->isContainer(parentClass))
        parentWidget.clear();

    const auto *cwi = m_uic->customWidgetsInfo();

    if (m_widgetChain.size() != 1) {
        m_output << m_indent << varName << " = " << language::operatorNew
            << language::fixClassName(cwi->realClassName(className))
            << '(' << parentWidget << ')' << language::eol;
    }

    parentWidget = savedParentWidget;


    if (cwi->extends(className, QLatin1String("QComboBox"))) {
        initializeComboBox(node);
    } else if (cwi->extends(className, QLatin1String("QListWidget"))) {
        initializeListWidget(node);
    } else if (cwi->extends(className, QLatin1String("QTreeWidget"))) {
        initializeTreeWidget(node);
    } else if (cwi->extends(className, QLatin1String("QTableWidget"))) {
        initializeTableWidget(node);
    }

    if (m_uic->isButton(className))
        addButtonGroup(node, varName);

    writeProperties(varName, className, node->elementProperty());

    if (!parentWidget.isEmpty()
        && cwi->extends(className, QLatin1String("QMenu"))) {
        initializeMenu(node, parentWidget);
    }

    if (node->elementLayout().isEmpty())
        m_layoutChain.push(0);

    m_layoutWidget = false;
    if (className == QLatin1String("QWidget") && !node->hasAttributeNative()) {
        if (const DomWidget* parentWidget = m_widgetChain.top()) {
            const QString parentClass = parentWidget->attributeClass();
            if (parentClass != QLatin1String("QMainWindow")
                && !m_uic->customWidgetsInfo()->isCustomWidgetContainer(parentClass)
                && !m_uic->isContainer(parentClass))
            m_layoutWidget = true;
        }
    }
    m_widgetChain.push(node);
    m_layoutChain.push(0);
    TreeWalker::acceptWidget(node);
    m_layoutChain.pop();
    m_widgetChain.pop();
    m_layoutWidget = false;

    const DomPropertyMap attributes = propertyMap(node->elementAttribute());

    const QString pageDefaultString = QLatin1String("Page");

    if (cwi->extends(parentClass, QLatin1String("QMainWindow"))) {
        if (cwi->extends(className, QLatin1String("QMenuBar"))) {
            m_output << m_indent << parentWidget << language::derefPointer
                << "setMenuBar(" << varName << ')' << language::eol;
        } else if (cwi->extends(className, QLatin1String("QToolBar"))) {
            m_output << m_indent << parentWidget << language::derefPointer << "addToolBar("
                << language::enumValue(toolBarAreaStringFromDOMAttributes(attributes)) << varName
                << ')' << language::eol;

            if (const DomProperty *pbreak = attributes.value(QLatin1String("toolBarBreak"))) {
                if (pbreak->elementBool() == QLatin1String("true")) {
                    m_output << m_indent << parentWidget << language::derefPointer
                        << "insertToolBarBreak(" <<  varName << ')' << language::eol;
                }
            }

        } else if (cwi->extends(className, QLatin1String("QDockWidget"))) {
            m_output << m_indent << parentWidget << language::derefPointer << "addDockWidget(";
            if (DomProperty *pstyle = attributes.value(QLatin1String("dockWidgetArea"))) {
                m_output << "Qt" << language::qualifier
                    << language::dockWidgetArea(pstyle->elementNumber()) << ", ";
            }
            m_output << varName << ")" << language::eol;
        } else if (m_uic->customWidgetsInfo()->extends(className, QLatin1String("QStatusBar"))) {
            m_output << m_indent << parentWidget << language::derefPointer
                << "setStatusBar(" << varName << ')' << language::eol;
        } else {
                m_output << m_indent << parentWidget << language::derefPointer
                    << "setCentralWidget(" << varName << ')' << language::eol;
        }
    }

    // Check for addPageMethod of a custom plugin first
    QString addPageMethod = cwi->customWidgetAddPageMethod(parentClass);
    if (addPageMethod.isEmpty())
        addPageMethod = cwi->simpleContainerAddPageMethod(parentClass);
    if (!addPageMethod.isEmpty()) {
        m_output << m_indent << parentWidget << language::derefPointer
            << addPageMethod << '(' << varName << ')' << language::eol;
    } else if (m_uic->customWidgetsInfo()->extends(parentClass, QLatin1String("QWizard"))) {
        addWizardPage(varName, node, parentWidget);
    } else if (m_uic->customWidgetsInfo()->extends(parentClass, QLatin1String("QToolBox"))) {
        const DomProperty *plabel = attributes.value(QLatin1String("label"));
        DomString *plabelString = plabel ? plabel->elementString() : nullptr;
        QString icon;
        if (const DomProperty *picon = attributes.value(QLatin1String("icon")))
            icon = QLatin1String(", ") + iconCall(picon); // Side effect: Writes icon definition
        m_output << m_indent << parentWidget << language::derefPointer << "addItem("
            << varName << icon << ", " << noTrCall(plabelString, pageDefaultString)
            << ')' << language::eol;

        autoTrOutput(plabelString, pageDefaultString) << m_indent << parentWidget
            << language::derefPointer << "setItemText(" << parentWidget
            << language::derefPointer << "indexOf("  << varName << "), "
            << autoTrCall(plabelString, pageDefaultString) << ')' << language::eol;

        if (DomProperty *ptoolTip = attributes.value(QLatin1String("toolTip"))) {
            autoTrOutput(ptoolTip->elementString())
                << language::openQtConfig(toolTipConfigKey())
                << m_indent << parentWidget << language::derefPointer << "setItemToolTip(" << parentWidget
                << language::derefPointer << "indexOf(" << varName << "), "
                << autoTrCall(ptoolTip->elementString()) << ')' << language::eol
                << language::closeQtConfig(toolTipConfigKey());
        }
    } else if (m_uic->customWidgetsInfo()->extends(parentClass, QLatin1String("QTabWidget"))) {
        const DomProperty *ptitle = attributes.value(QLatin1String("title"));
        DomString *ptitleString = ptitle ? ptitle->elementString() : nullptr;
        QString icon;
        if (const DomProperty *picon = attributes.value(QLatin1String("icon")))
            icon = QLatin1String(", ") + iconCall(picon); // Side effect: Writes icon definition
        m_output << m_indent << parentWidget << language::derefPointer << "addTab("
            << varName << icon << ", " << language::emptyString << ')' << language::eol;

        autoTrOutput(ptitleString, pageDefaultString) << m_indent << parentWidget
            << language::derefPointer << "setTabText(" << parentWidget
            << language::derefPointer << "indexOf(" << varName << "), "
            << autoTrCall(ptitleString, pageDefaultString) << ')' << language::eol;

        if (const DomProperty *ptoolTip = attributes.value(QLatin1String("toolTip"))) {
            autoTrOutput(ptoolTip->elementString())
                << language::openQtConfig(toolTipConfigKey())
                << m_indent << parentWidget << language::derefPointer << "setTabToolTip("
                << parentWidget << language::derefPointer << "indexOf(" << varName
                << "), " << autoTrCall(ptoolTip->elementString()) << ')' << language::eol
                << language::closeQtConfig(toolTipConfigKey());
        }
        if (const DomProperty *pwhatsThis = attributes.value(QLatin1String("whatsThis"))) {
            autoTrOutput(pwhatsThis->elementString())
                << language::openQtConfig(whatsThisConfigKey())
                << m_indent << parentWidget << language::derefPointer << "setTabWhatsThis("
                << parentWidget << language::derefPointer << "indexOf(" << varName
                << "), " << autoTrCall(pwhatsThis->elementString()) << ')' << language::eol
                << language::closeQtConfig(whatsThisConfigKey());
        }
    }

    //
    // Special handling for qtableview/qtreeview fake header attributes
    //
    static const QLatin1String realPropertyNames[] = {
        QLatin1String("visible"),
        QLatin1String("cascadingSectionResizes"),
        QLatin1String("minimumSectionSize"),    // before defaultSectionSize
        QLatin1String("defaultSectionSize"),
        QLatin1String("highlightSections"),
        QLatin1String("showSortIndicator"),
        QLatin1String("stretchLastSection"),
    };

    static const QStringList trees = {
        QLatin1String("QTreeView"), QLatin1String("QTreeWidget")
    };
    static const QStringList tables = {
        QLatin1String("QTableView"), QLatin1String("QTableWidget")
    };

    if (cwi->extendsOneOf(className, trees)) {
        DomPropertyList headerProperties;
        for (auto realPropertyName : realPropertyNames) {
            const QString fakePropertyName = QLatin1String("header")
                    + QChar(realPropertyName.at(0)).toUpper() + realPropertyName.mid(1);
            if (DomProperty *fakeProperty = attributes.value(fakePropertyName)) {
                fakeProperty->setAttributeName(realPropertyName);
                headerProperties << fakeProperty;
            }
        }
        writeProperties(varName + language::derefPointer + QLatin1String("header()"),
                        QLatin1String("QHeaderView"), headerProperties,
                        WritePropertyIgnoreObjectName);

    } else if (cwi->extendsOneOf(className, tables)) {
        static const QLatin1String headerPrefixes[] = {
            QLatin1String("horizontalHeader"),
            QLatin1String("verticalHeader"),
        };

        for (auto headerPrefix : headerPrefixes) {
            DomPropertyList headerProperties;
            for (auto realPropertyName : realPropertyNames) {
                const QString fakePropertyName = headerPrefix
                        + QChar(realPropertyName.at(0)).toUpper() + realPropertyName.mid(1);
                if (DomProperty *fakeProperty = attributes.value(fakePropertyName)) {
                    fakeProperty->setAttributeName(realPropertyName);
                    headerProperties << fakeProperty;
                }
            }
            const QString headerVar = varName + language::derefPointer
                + headerPrefix + QLatin1String("()");
            writeProperties(headerVar, QLatin1String("QHeaderView"),
                            headerProperties, WritePropertyIgnoreObjectName);
        }
    }

    if (node->elementLayout().isEmpty())
        m_layoutChain.pop();

    const QStringList zOrder = node->elementZOrder();
    for (const QString &name : zOrder) {
        const QString varName = m_driver->widgetVariableName(name);
        if (varName.isEmpty()) {
            fprintf(stderr, "%s: Warning: Z-order assignment: '%s' is not a valid widget.\n",
                    qPrintable(m_option.messagePrefix()),
                    name.toLatin1().data());
        } else {
            m_output << m_indent << varName << language::derefPointer
                << (language::language() != Language::Python ? "raise()" : "raise_()") << language::eol;
        }
    }
}

void WriteInitialization::addButtonGroup(const DomWidget *buttonNode, const QString &varName)
{
    const DomPropertyMap attributes = propertyMap(buttonNode->elementAttribute());
    // Look up the button group name as specified in the attribute and find the uniquified name
    const DomProperty *prop = attributes.value(QLatin1String("buttonGroup"));
    if (!prop)
        return;
    const QString attributeName = toString(prop->elementString());
    const DomButtonGroup *group = m_driver->findButtonGroup(attributeName);
    // Legacy feature: Create missing groups on the fly as the UIC button group feature
    // was present before the actual Designer support (4.5)
    const bool createGroupOnTheFly = group == nullptr;
    if (createGroupOnTheFly) {
        DomButtonGroup *newGroup = new DomButtonGroup;
        newGroup->setAttributeName(attributeName);
        group = newGroup;
        fprintf(stderr, "%s: Warning: Creating button group `%s'\n",
                qPrintable(m_option.messagePrefix()),
                attributeName.toLatin1().data());
    }
    const QString groupName = m_driver->findOrInsertButtonGroup(group);
    // Create on demand
    if (!m_buttonGroups.contains(groupName)) {
        const QString className = QLatin1String("QButtonGroup");
        m_output << m_indent;
        if (createGroupOnTheFly)
            m_output << className << " *";
        m_output << groupName << " = " << language::operatorNew
            << className << '(' << m_mainFormVarName << ')' << language::eol;
        m_buttonGroups.insert(groupName);
        writeProperties(groupName, className, group->elementProperty());
    }
    m_output << m_indent << groupName << language::derefPointer << "addButton("
        << varName << ')' << language::eol;
}

void WriteInitialization::acceptLayout(DomLayout *node)
{
    const QString className = node->attributeClass();
    const QString varName = m_driver->findOrInsertLayout(node);

    const DomPropertyMap properties = propertyMap(node->elementProperty());
    const bool oldLayoutProperties = properties.value(QLatin1String("margin")) != nullptr;

    bool isGroupBox = false;

    m_output << m_indent << varName << " = " << language::operatorNew << className << '(';

    if (!m_layoutChain.top() && !isGroupBox)
        m_output << m_driver->findOrInsertWidget(m_widgetChain.top());

    m_output << ")" << language::eol;

    // Suppress margin on a read child layout
    const bool suppressMarginDefault = m_layoutChain.top();
    int marginType = Use43UiFile;
    if (oldLayoutProperties)
        marginType = m_layoutMarginType;
    m_LayoutDefaultHandler.writeProperties(m_indent, varName, properties, marginType, suppressMarginDefault, m_output);

    m_layoutMarginType = SubLayoutMargin;

    DomPropertyList propList = node->elementProperty();
    DomPropertyList newPropList;
    if (m_layoutWidget) {
        bool left, top, right, bottom;
        left = top = right = bottom = false;
        for (const DomProperty *p : propList) {
            const QString propertyName = p->attributeName();
            if (propertyName == QLatin1String("leftMargin") && p->kind() == DomProperty::Number)
                left = true;
            else if (propertyName == QLatin1String("topMargin") && p->kind() == DomProperty::Number)
                top = true;
            else if (propertyName == QLatin1String("rightMargin") && p->kind() == DomProperty::Number)
                right = true;
            else if (propertyName == QLatin1String("bottomMargin") && p->kind() == DomProperty::Number)
                bottom = true;
        }
        if (!left) {
            DomProperty *p = new DomProperty();
            p->setAttributeName(QLatin1String("leftMargin"));
            p->setElementNumber(0);
            newPropList.append(p);
        }
        if (!top) {
            DomProperty *p = new DomProperty();
            p->setAttributeName(QLatin1String("topMargin"));
            p->setElementNumber(0);
            newPropList.append(p);
        }
        if (!right) {
            DomProperty *p = new DomProperty();
            p->setAttributeName(QLatin1String("rightMargin"));
            p->setElementNumber(0);
            newPropList.append(p);
        }
        if (!bottom) {
            DomProperty *p = new DomProperty();
            p->setAttributeName(QLatin1String("bottomMargin"));
            p->setElementNumber(0);
            newPropList.append(p);
        }
        m_layoutWidget = false;
    }

    propList.append(newPropList);

    writeProperties(varName, className, propList, WritePropertyIgnoreMargin|WritePropertyIgnoreSpacing);

    // Clean up again:
    propList.clear();
    qDeleteAll(newPropList);
    newPropList.clear();

    m_layoutChain.push(node);
    TreeWalker::acceptLayout(node);
    m_layoutChain.pop();

    // Stretch? (Unless we are compiling for UIC3)
    const QString numberNull = QString(QLatin1Char('0'));
    writePropertyList(varName, QLatin1String("setStretch"), node->attributeStretch(), numberNull);
    writePropertyList(varName, QLatin1String("setRowStretch"), node->attributeRowStretch(), numberNull);
    writePropertyList(varName, QLatin1String("setColumnStretch"), node->attributeColumnStretch(), numberNull);
    writePropertyList(varName, QLatin1String("setColumnMinimumWidth"), node->attributeColumnMinimumWidth(), numberNull);
    writePropertyList(varName, QLatin1String("setRowMinimumHeight"), node->attributeRowMinimumHeight(), numberNull);
}

// Apply a comma-separated list of values using a function "setSomething(int idx, value)"
void WriteInitialization::writePropertyList(const QString &varName,
                                            const QString &setFunction,
                                            const QString &value,
                                            const QString &defaultValue)
{
    if (value.isEmpty())
        return;
    const QStringList list = value.split(QLatin1Char(','));
    const int count =  list.count();
    for (int i = 0; i < count; i++) {
        if (list.at(i) != defaultValue) {
            m_output << m_indent << varName << language::derefPointer << setFunction
                << '(' << i << ", " << list.at(i) << ')' << language::eol;
        }
    }
}

void WriteInitialization::acceptSpacer(DomSpacer *node)
{
    m_output << m_indent << m_driver->findOrInsertSpacer(node) << " = ";
    writeSpacerItem(node, m_output);
    m_output << language::eol;
}

static inline QString formLayoutRole(int column, int colspan)
{
    if (colspan > 1)
        return QLatin1String("QFormLayout::SpanningRole");
    return column == 0 ? QLatin1String("QFormLayout::LabelRole") : QLatin1String("QFormLayout::FieldRole");
}

static QString layoutAddMethod(DomLayoutItem::Kind kind, const QString &layoutClass)
{
    const QString methodPrefix = layoutClass == QLatin1String("QFormLayout")
        ? QLatin1String("set") : QLatin1String("add");
    switch (kind) {
    case DomLayoutItem::Widget:
        return methodPrefix + QLatin1String("Widget");
    case DomLayoutItem::Layout:
        return methodPrefix + QLatin1String("Layout");
    case DomLayoutItem::Spacer:
        return methodPrefix + QLatin1String("Item");
    case DomLayoutItem::Unknown:
        Q_ASSERT( false );
        break;
    }
    Q_UNREACHABLE();
}

void WriteInitialization::acceptLayoutItem(DomLayoutItem *node)
{
    TreeWalker::acceptLayoutItem(node);

    DomLayout *layout = m_layoutChain.top();

    if (!layout)
        return;

    const QString layoutName = m_driver->findOrInsertLayout(layout);
    const QString itemName = m_driver->findOrInsertLayoutItem(node);

    m_output << "\n" << m_indent << layoutName << language::derefPointer << ""
        << layoutAddMethod(node->kind(), layout->attributeClass()) << '(';

    if (layout->attributeClass() == QLatin1String("QGridLayout")) {
        const int row = node->attributeRow();
        const int col = node->attributeColumn();

        const int rowSpan = node->hasAttributeRowSpan() ? node->attributeRowSpan() : 1;
        const int colSpan = node->hasAttributeColSpan() ? node->attributeColSpan() : 1;
        m_output << itemName << ", " << row << ", " << col << ", " << rowSpan << ", " << colSpan;
        if (!node->attributeAlignment().isEmpty())
            m_output << ", " << language::enumValue(node->attributeAlignment());
    } else if (layout->attributeClass() == QLatin1String("QFormLayout")) {
        const int row = node->attributeRow();
        const int colSpan = node->hasAttributeColSpan() ? node->attributeColSpan() : 1;
        const QString role = formLayoutRole(node->attributeColumn(), colSpan);
        m_output << row << ", " << language::enumValue(role) << ", " << itemName;
    } else {
        m_output << itemName;
        if (layout->attributeClass().contains(QLatin1String("Box")) && !node->attributeAlignment().isEmpty())
            m_output << ", 0, " << language::enumValue(node->attributeAlignment());
    }
    m_output << ")" << language::eol << "\n";
}

void WriteInitialization::acceptActionGroup(DomActionGroup *node)
{
    const QString actionName = m_driver->findOrInsertActionGroup(node);
    QString varName = m_driver->findOrInsertWidget(m_widgetChain.top());

    if (m_actionGroupChain.top())
        varName = m_driver->findOrInsertActionGroup(m_actionGroupChain.top());

    m_output << m_indent << actionName << " = " << language::operatorNew
        << "QActionGroup(" << varName << ")" << language::eol;
    writeProperties(actionName, QLatin1String("QActionGroup"), node->elementProperty());

    m_actionGroupChain.push(node);
    TreeWalker::acceptActionGroup(node);
    m_actionGroupChain.pop();
}

void WriteInitialization::acceptAction(DomAction *node)
{
    if (node->hasAttributeMenu())
        return;

    const QString actionName = m_driver->findOrInsertAction(node);
    QString varName = m_driver->findOrInsertWidget(m_widgetChain.top());

    if (m_actionGroupChain.top())
        varName = m_driver->findOrInsertActionGroup(m_actionGroupChain.top());

    m_output << m_indent << actionName << " = " << language::operatorNew
        << "QAction(" << varName << ')' << language::eol;
    writeProperties(actionName, QLatin1String("QAction"), node->elementProperty());
}

void WriteInitialization::acceptActionRef(DomActionRef *node)
{
    QString actionName = node->attributeName();
    if (actionName.isEmpty() || !m_widgetChain.top()
        || m_driver->actionGroupByName(actionName)) {
        return;
    }

    const QString varName = m_driver->findOrInsertWidget(m_widgetChain.top());

    if (m_widgetChain.top() && actionName == QLatin1String("separator")) {
        // separator is always reserved!
        m_actionOut << m_indent << varName << language::derefPointer
            << "addSeparator()" << language::eol;
        return;
    }

    const DomWidget *domWidget = m_driver->widgetByName(actionName);
    if (domWidget && m_uic->isMenu(domWidget->attributeClass())) {
        m_actionOut << m_indent << varName << language::derefPointer
            << "addAction(" << m_driver->findOrInsertWidget(domWidget)
            << language::derefPointer << "menuAction())" << language::eol;
        return;
    }

    const DomAction *domAction = m_driver->actionByName(actionName);
    if (!domAction) {
        fprintf(stderr, "%s: Warning: action `%s' not declared\n",
                qPrintable(m_option.messagePrefix()), qPrintable(actionName));
        return;
    }

    m_actionOut << m_indent << varName << language::derefPointer
        << "addAction(" << m_driver->findOrInsertAction(domAction)
        << ')' << language::eol;
}

QString WriteInitialization::writeStringListProperty(const DomStringList *list) const
{
    QString propertyValue;
    QTextStream str(&propertyValue);
    str << "QStringList()";
    const QStringList values = list->elementString();
    if (values.isEmpty())
        return propertyValue;
    if (needsTranslation(list)) {
        const QString comment = list->attributeComment();
        for (int i = 0; i < values.size(); ++i)
            str << '\n' << m_indent << "    << " << trCall(values.at(i), comment);
    } else {
        for (int i = 0; i < values.size(); ++i)
            str << " << " << language::qstring(values.at(i), m_dindent);
    }
    return propertyValue;
}

static QString configKeyForProperty(const QString &propertyName)
{
    if (propertyName == QLatin1String("toolTip"))
        return toolTipConfigKey();
    if (propertyName == QLatin1String("whatsThis"))
        return whatsThisConfigKey();
    if (propertyName == QLatin1String("statusTip"))
        return statusTipConfigKey();
    if (propertyName == QLatin1String("shortcut"))
        return shortcutConfigKey();
    if (propertyName == QLatin1String("accessibleName")
        || propertyName == QLatin1String("accessibleDescription")) {
        return accessibilityConfigKey();
    }
    return QString();
}

void WriteInitialization::writeProperties(const QString &varName,
                                          const QString &className,
                                          const DomPropertyList &lst,
                                          unsigned flags)
{
    const bool isTopLevel = m_widgetChain.count() == 1;

    if (m_uic->customWidgetsInfo()->extends(className, QLatin1String("QAxWidget"))) {
        DomPropertyMap properties = propertyMap(lst);
        if (DomProperty *p = properties.value(QLatin1String("control"))) {
            m_output << m_indent << varName << language::derefPointer << "setControl("
                << language::qstring(toString(p->elementString()), m_dindent)
                << ')' << language::eol;
        }
    }

    QString indent;
    if (!m_widgetChain.top()) {
        indent = m_option.indent;
        switch (language::language()) {
         case Language::Cpp:
            m_output << m_indent << "if (" << varName << "->objectName().isEmpty())\n";
            break;
        case Language::Python:
           m_output << m_indent << "if not " << varName << ".objectName():\n";
           break;
        }
    }
    if (!(flags & WritePropertyIgnoreObjectName)) {
        QString objectName = varName;
        if (!language::self.isEmpty() && objectName.startsWith(language::self))
            objectName.remove(0, language::self.size());
        m_output << m_indent << indent
            << varName << language::derefPointer << "setObjectName("
            << language::qstring(objectName, m_dindent) << ')' << language::eol;
    }

    int leftMargin, topMargin, rightMargin, bottomMargin;
    leftMargin = topMargin = rightMargin = bottomMargin = -1;
    bool frameShadowEncountered = false;

    for (const DomProperty *p : lst) {
        if (!checkProperty(m_option.inputFile, p))
            continue;
        QString propertyName = p->attributeName();
        QString propertyValue;
        bool delayProperty = false;

        // special case for the property `geometry': Do not use position
        if (isTopLevel && propertyName == QLatin1String("geometry") && p->elementRect()) {
            const DomRect *r = p->elementRect();
            m_output << m_indent << varName << language::derefPointer << "resize("
                << r->elementWidth() << ", " << r->elementHeight() << ')' << language::eol;
            continue;
        }
        if (propertyName == QLatin1String("currentRow") // QListWidget::currentRow
                && m_uic->customWidgetsInfo()->extends(className, QLatin1String("QListWidget"))) {
            m_delayedOut << m_indent << varName << language::derefPointer
                << "setCurrentRow(" << p->elementNumber() << ')' << language::eol;
            continue;
        }
        static const QStringList currentIndexWidgets = {
            QLatin1String("QComboBox"), QLatin1String("QStackedWidget"),
            QLatin1String("QTabWidget"), QLatin1String("QToolBox")
        };
        if (propertyName == QLatin1String("currentIndex") // set currentIndex later
            && (m_uic->customWidgetsInfo()->extendsOneOf(className, currentIndexWidgets))) {
            m_delayedOut << m_indent << varName << language::derefPointer
                << "setCurrentIndex(" << p->elementNumber() << ')' << language::eol;
            continue;
        }
        if (propertyName == QLatin1String("tabSpacing")
            && m_uic->customWidgetsInfo()->extends(className, QLatin1String("QToolBox"))) {
            m_delayedOut << m_indent << varName << language::derefPointer
                << "layout()" << language::derefPointer << "setSpacing("
                << p->elementNumber() << ')' << language::eol;
            continue;
        }
        if (propertyName == QLatin1String("control") // ActiveQt support
            && m_uic->customWidgetsInfo()->extends(className, QLatin1String("QAxWidget"))) {
            // already done ;)
            continue;
        }
        if (propertyName == QLatin1String("default")
            && m_uic->customWidgetsInfo()->extends(className, QLatin1String("QPushButton"))) {
            // QTBUG-44406: Setting of QPushButton::default needs to be delayed until the parent is set
            delayProperty = true;
        } else if (propertyName == QLatin1String("database")
                    && p->elementStringList()) {
            // Sql support
            continue;
        } else if (propertyName == QLatin1String("frameworkCode")
                    && p->kind() == DomProperty::Bool) {
            // Sql support
            continue;
        } else if (propertyName == QLatin1String("orientation")
                    && m_uic->customWidgetsInfo()->extends(className, QLatin1String("Line"))) {
            // Line support
            QString shape = QLatin1String("QFrame::HLine");
            if (p->elementEnum() == QLatin1String("Qt::Vertical"))
                shape = QLatin1String("QFrame::VLine");

            m_output << m_indent << varName << language::derefPointer << "setFrameShape("
                << language::enumValue(shape) << ')' << language::eol;
            // QFrame Default is 'Plain'. Make the line 'Sunken' unless otherwise specified
            if (!frameShadowEncountered) {
                m_output << m_indent << varName << language::derefPointer
                    << "setFrameShadow("
                    << language::enumValue(QLatin1String("QFrame::Sunken"))
                    << ')' << language::eol;
            }
            continue;
        } else if ((flags & WritePropertyIgnoreMargin)  && propertyName == QLatin1String("margin")) {
            continue;
        } else if ((flags & WritePropertyIgnoreSpacing) && propertyName == QLatin1String("spacing")) {
            continue;
        } else if (propertyName == QLatin1String("leftMargin") && p->kind() == DomProperty::Number) {
            leftMargin = p->elementNumber();
            continue;
        } else if (propertyName == QLatin1String("topMargin") && p->kind() == DomProperty::Number) {
            topMargin = p->elementNumber();
            continue;
        } else if (propertyName == QLatin1String("rightMargin") && p->kind() == DomProperty::Number) {
            rightMargin = p->elementNumber();
            continue;
        } else if (propertyName == QLatin1String("bottomMargin") && p->kind() == DomProperty::Number) {
            bottomMargin = p->elementNumber();
            continue;
        } else if (propertyName == QLatin1String("numDigits") // Deprecated in Qt 4, removed in Qt 5.
                   && m_uic->customWidgetsInfo()->extends(className, QLatin1String("QLCDNumber"))) {
            qWarning("Widget '%s': Deprecated property QLCDNumber::numDigits encountered. It has been replaced by QLCDNumber::digitCount.",
                     qPrintable(varName));
            propertyName = QLatin1String("digitCount");
        } else if (propertyName == QLatin1String("frameShadow"))
            frameShadowEncountered = true;

        bool stdset = m_stdsetdef;
        if (p->hasAttributeStdset())
            stdset = p->attributeStdset();

        QString setFunction;

        {
            QTextStream str(&setFunction);
            if (stdset) {
                str << language::derefPointer <<"set" << propertyName.at(0).toUpper()
                    << QStringView{propertyName}.mid(1) << '(';
            } else {
                str << language::derefPointer << QLatin1String("setProperty(\"")
                    << propertyName << "\", ";
                if (language::language() == Language::Cpp) {
                    str << "QVariant";
                    if (p->kind() == DomProperty::Enum)
                        str << "::fromValue";
                    str << '(';
                }
            }
        } // QTextStream

        QString varNewName = varName;

        switch (p->kind()) {
        case DomProperty::Bool: {
            propertyValue = language::boolValue(p->elementBool() == language::cppTrue);
            break;
        }
        case DomProperty::Color:
            propertyValue = domColor2QString(p->elementColor());
            break;
        case DomProperty::Cstring:
            if (propertyName == QLatin1String("buddy") && m_uic->customWidgetsInfo()->extends(className, QLatin1String("QLabel"))) {
                Buddy buddy = { varName, p->elementCstring() };
                m_buddies.append(std::move(buddy));
            } else {
                QTextStream str(&propertyValue);
                if (!stdset)
                    str << "QByteArray(";
                str << language::charliteral(p->elementCstring(), m_dindent);
                if (!stdset)
                    str << ')';
            }
            break;
        case DomProperty::Cursor:
            propertyValue = QString::fromLatin1("QCursor(static_cast<Qt::CursorShape>(%1))")
                            .arg(p->elementCursor());
            break;
        case DomProperty::CursorShape:
            if (p->hasAttributeStdset() && !p->attributeStdset())
                varNewName += language::derefPointer + QLatin1String("viewport()");
            propertyValue = QLatin1String("QCursor(Qt") + language::qualifier
                + p->elementCursorShape() + QLatin1Char(')');
            break;
        case DomProperty::Enum:
            propertyValue = p->elementEnum();
            if (propertyValue.contains(language::cppQualifier))
                propertyValue = language::enumValue(propertyValue);
            else
                propertyValue.prepend(className + language::qualifier);
            break;
        case DomProperty::Set:
            propertyValue = language::enumValue(p->elementSet());
            break;
        case DomProperty::Font:
            propertyValue = writeFontProperties(p->elementFont());
            break;
        case DomProperty::IconSet:
            propertyValue = writeIconProperties(p->elementIconSet());
            break;
        case DomProperty::Pixmap:
            propertyValue = pixCall(p);
            break;
        case DomProperty::Palette: {
            const DomPalette *pal = p->elementPalette();
            const QString paletteName = m_driver->unique(QLatin1String("palette"));
            m_output << m_indent << language::stackVariable("QPalette", paletteName)
                << language::eol;
            writeColorGroup(pal->elementActive(), QLatin1String("QPalette::Active"), paletteName);
            writeColorGroup(pal->elementInactive(), QLatin1String("QPalette::Inactive"), paletteName);
            writeColorGroup(pal->elementDisabled(), QLatin1String("QPalette::Disabled"), paletteName);

            propertyValue = paletteName;
            break;
        }
        case DomProperty::Point: {
            const DomPoint *po = p->elementPoint();
            propertyValue = QString::fromLatin1("QPoint(%1, %2)")
                            .arg(po->elementX()).arg(po->elementY());
            break;
        }
        case DomProperty::PointF: {
            const DomPointF *pof = p->elementPointF();
            propertyValue = QString::fromLatin1("QPointF(%1, %2)")
                            .arg(pof->elementX()).arg(pof->elementY());
            break;
        }
        case DomProperty::Rect: {
            const DomRect *r = p->elementRect();
            propertyValue = QString::fromLatin1("QRect(%1, %2, %3, %4)")
                            .arg(r->elementX()).arg(r->elementY())
                            .arg(r->elementWidth()).arg(r->elementHeight());
            break;
        }
        case DomProperty::RectF: {
            const DomRectF *rf = p->elementRectF();
            propertyValue = QString::fromLatin1("QRectF(%1, %2, %3, %4)")
                            .arg(rf->elementX()).arg(rf->elementY())
                            .arg(rf->elementWidth()).arg(rf->elementHeight());
            break;
        }
        case DomProperty::Locale: {
             const DomLocale *locale = p->elementLocale();
             QTextStream(&propertyValue) << "QLocale(QLocale" << language::qualifier
                 << locale->attributeLanguage() << ", QLocale" << language::qualifier
                 << locale->attributeCountry() << ')';
            break;
        }
        case DomProperty::SizePolicy: {
            const QString spName = writeSizePolicy( p->elementSizePolicy());
            m_output << m_indent << spName << ".setHeightForWidth("
                << varName << language::derefPointer << "sizePolicy().hasHeightForWidth())"
                << language::eol;

            propertyValue = spName;
            break;
        }
        case DomProperty::Size: {
             const DomSize *s = p->elementSize();
              propertyValue = QString::fromLatin1("QSize(%1, %2)")
                             .arg(s->elementWidth()).arg(s->elementHeight());
            break;
        }
        case DomProperty::SizeF: {
            const DomSizeF *sf = p->elementSizeF();
             propertyValue = QString::fromLatin1("QSizeF(%1, %2)")
                            .arg(sf->elementWidth()).arg(sf->elementHeight());
            break;
        }
        case DomProperty::String: {
            if (propertyName == QLatin1String("objectName")) {
                const QString v = p->elementString()->text();
                if (v == varName)
                    break;

                // ### qWarning("Deprecated: the property `objectName' is different from the variable name");
            }

            propertyValue = autoTrCall(p->elementString());
            break;
        }
        case DomProperty::Number:
            propertyValue = QString::number(p->elementNumber());
            break;
        case DomProperty::UInt:
            propertyValue = QString::number(p->elementUInt());
            propertyValue += QLatin1Char('u');
            break;
        case DomProperty::LongLong:
            propertyValue = QLatin1String("Q_INT64_C(");
            propertyValue += QString::number(p->elementLongLong());
            propertyValue += QLatin1Char(')');;
            break;
        case DomProperty::ULongLong:
            propertyValue = QLatin1String("Q_UINT64_C(");
            propertyValue += QString::number(p->elementULongLong());
            propertyValue += QLatin1Char(')');
            break;
        case DomProperty::Float:
            propertyValue = QString::number(p->elementFloat(), 'f', 8);
            break;
        case DomProperty::Double:
            propertyValue = QString::number(p->elementDouble(), 'f', 15);
            break;
        case DomProperty::Char: {
            const DomChar *c = p->elementChar();
            propertyValue = QString::fromLatin1("QChar(%1)")
                            .arg(c->elementUnicode());
            break;
        }
        case DomProperty::Date: {
            const DomDate *d = p->elementDate();
            propertyValue = QString::fromLatin1("QDate(%1, %2, %3)")
                            .arg(d->elementYear())
                            .arg(d->elementMonth())
                            .arg(d->elementDay());
            break;
        }
        case DomProperty::Time: {
            const DomTime *t = p->elementTime();
            propertyValue = QString::fromLatin1("QTime(%1, %2, %3)")
                            .arg(t->elementHour())
                            .arg(t->elementMinute())
                            .arg(t->elementSecond());
            break;
        }
        case DomProperty::DateTime: {
            const DomDateTime *dt = p->elementDateTime();
            propertyValue = QString::fromLatin1("QDateTime(QDate(%1, %2, %3), QTime(%4, %5, %6))")
                            .arg(dt->elementYear())
                            .arg(dt->elementMonth())
                            .arg(dt->elementDay())
                            .arg(dt->elementHour())
                            .arg(dt->elementMinute())
                            .arg(dt->elementSecond());
            break;
        }
        case DomProperty::StringList:
            propertyValue = writeStringListProperty(p->elementStringList());
            break;

        case DomProperty::Url: {
            const DomUrl* u = p->elementUrl();
            QTextStream(&propertyValue) << "QUrl("
                << language::qstring(u->elementString()->text(), m_dindent) << ")";
            break;
        }
        case DomProperty::Brush:
            propertyValue = writeBrushInitialization(p->elementBrush());
            break;
        case DomProperty::Unknown:
            break;
        }

        if (!propertyValue.isEmpty()) {
            const QString configKey = configKeyForProperty(propertyName);

            QTextStream &o = delayProperty ? m_delayedOut : autoTrOutput(p);

            if (!configKey.isEmpty())
                o << language::openQtConfig(configKey);
            o << m_indent << varNewName << setFunction << propertyValue;
            if (!stdset && language::language() == Language::Cpp)
                o << ')';
            o << ')' << language::eol;
            if (!configKey.isEmpty())
               o << language::closeQtConfig(configKey);

            if (varName == m_mainFormVarName && &o == &m_refreshOut) {
                // this is the only place (currently) where we output mainForm name to the retranslateUi().
                // Other places output merely instances of a certain class (which cannot be main form, e.g. QListWidget).
                m_mainFormUsedInRetranslateUi = true;
            }
        }
    }
    if (leftMargin != -1 || topMargin != -1 || rightMargin != -1 || bottomMargin != -1) {
        m_output << m_indent << varName << language::derefPointer << "setContentsMargins("
            << leftMargin << ", " << topMargin << ", "
            << rightMargin << ", " << bottomMargin << ")" << language::eol;
    }
}

QString  WriteInitialization::writeSizePolicy(const DomSizePolicy *sp)
{

    // check cache
    const SizePolicyHandle sizePolicyHandle(sp);
    const SizePolicyNameMap::const_iterator it = m_sizePolicyNameMap.constFind(sizePolicyHandle);
    if ( it != m_sizePolicyNameMap.constEnd()) {
        return it.value();
    }


    // insert with new name
    const QString spName = m_driver->unique(QLatin1String("sizePolicy"));
    m_sizePolicyNameMap.insert(sizePolicyHandle, spName);

    m_output << m_indent << language::stackVariableWithInitParameters("QSizePolicy", spName);
    if (sp->hasElementHSizeType() && sp->hasElementVSizeType()) {
        m_output << "QSizePolicy" << language::qualifier << language::sizePolicy(sp->elementHSizeType())
            << ", QSizePolicy" << language::qualifier << language::sizePolicy(sp->elementVSizeType());
    } else if (sp->hasAttributeHSizeType() && sp->hasAttributeVSizeType()) {
        m_output << "QSizePolicy" << language::qualifier << sp->attributeHSizeType()
            << ", QSizePolicy" << language::qualifier << sp->attributeVSizeType();
    }
    m_output << ')' << language::eol;

    m_output << m_indent << spName << ".setHorizontalStretch("
        << sp->elementHorStretch() << ")" << language::eol;
    m_output << m_indent << spName << ".setVerticalStretch("
        << sp->elementVerStretch() << ")" << language::eol;
    return spName;
}
// Check for a font with the given properties in the FontPropertiesNameMap
// or create a new one. Returns the name.

QString WriteInitialization::writeFontProperties(const DomFont *f)
{
    // check cache
    const FontHandle fontHandle(f);
    const FontPropertiesNameMap::const_iterator it = m_fontPropertiesNameMap.constFind(fontHandle);
    if ( it != m_fontPropertiesNameMap.constEnd()) {
        return it.value();
    }

    // insert with new name
    const QString fontName = m_driver->unique(QLatin1String("font"));
    m_fontPropertiesNameMap.insert(FontHandle(f), fontName);

    m_output << m_indent << language::stackVariable("QFont", fontName)
        << language::eol;
    if (f->hasElementFamily() && !f->elementFamily().isEmpty()) {
        m_output << m_indent << fontName << ".setFamily("
            << language::qstring(f->elementFamily(), m_dindent) << ")" << language::eol;
    }
    if (f->hasElementPointSize() && f->elementPointSize() > 0) {
         m_output << m_indent << fontName << ".setPointSize(" << f->elementPointSize()
             << ")" << language::eol;
    }

    if (f->hasElementBold()) {
        m_output << m_indent << fontName << ".setBold("
            << language::boolValue(f->elementBold()) << ')' << language::eol;
    }
    if (f->hasElementItalic()) {
        m_output << m_indent << fontName << ".setItalic("
            << language::boolValue(f->elementItalic()) << ')' << language::eol;
    }
    if (f->hasElementUnderline()) {
        m_output << m_indent << fontName << ".setUnderline("
            << language::boolValue(f->elementUnderline()) << ')' << language::eol;
    }
    if (f->hasElementWeight() && f->elementWeight() > 0) {
        m_output << m_indent << fontName << ".setWeight("
            << f->elementWeight() << ")" << language::eol;
    }
    if (f->hasElementStrikeOut()) {
         m_output << m_indent << fontName << ".setStrikeOut("
            << language::boolValue(f->elementStrikeOut()) << ')' << language::eol;
    }
    if (f->hasElementKerning()) {
        m_output << m_indent << fontName << ".setKerning("
            << language::boolValue(f->elementKerning()) << ')' << language::eol;
    }
    if (f->hasElementAntialiasing()) {
        m_output << m_indent << fontName << ".setStyleStrategy(QFont"
            << language::qualifier
            << (f->elementAntialiasing() ? "PreferDefault" : "NoAntialias")
            << ')' << language::eol;
    }
    if (f->hasElementStyleStrategy()) {
         m_output << m_indent << fontName << ".setStyleStrategy(QFont"
            << language::qualifier << f->elementStyleStrategy() << ')' << language::eol;
    }
    return  fontName;
}

static void writeIconAddFile(QTextStream &output, const QString &indent,
                             const QString &iconName, const QString &fileName,
                             const char *mode, const char *state)
{
    output << indent << iconName << ".addFile("
        << language::qstring(fileName, indent) << ", QSize(), QIcon"
        << language::qualifier << mode << ", QIcon" << language::qualifier
        << state << ')' << language::eol;
}

// Post 4.4 write resource icon
static void writeResourceIcon(QTextStream &output,
                              const QString &iconName,
                              const QString &indent,
                              const DomResourceIcon *i)
{
    if (i->hasElementNormalOff()) {
        writeIconAddFile(output, indent, iconName, i->elementNormalOff()->text(),
                         "Normal", "Off");
    }
    if (i->hasElementNormalOn()) {
        writeIconAddFile(output, indent, iconName, i->elementNormalOn()->text(),
                         "Normal", "On");
    }
    if (i->hasElementDisabledOff()) {
        writeIconAddFile(output, indent, iconName, i->elementDisabledOff()->text(),
                         "Disabled", "Off");
    }
    if (i->hasElementDisabledOn()) {
        writeIconAddFile(output, indent, iconName, i->elementDisabledOn()->text(),
                         "Disabled", "On");
    }
    if (i->hasElementActiveOff()) {
        writeIconAddFile(output, indent, iconName, i->elementActiveOff()->text(),
                         "Active", "Off");
    }
    if (i->hasElementActiveOn()) {
        writeIconAddFile(output, indent, iconName, i->elementActiveOn()->text(),
                         "Active", "On");
    }
    if (i->hasElementSelectedOff()) {
        writeIconAddFile(output, indent, iconName, i->elementSelectedOff()->text(),
                         "Selected", "Off");
    }
    if (i->hasElementSelectedOn()) {
        writeIconAddFile(output, indent, iconName, i->elementSelectedOn()->text(),
                         "Selected", "On");
    }
}

static void writeIconAddPixmap(QTextStream &output, const QString &indent,
                               const QString &iconName, const QString &call,
                               const char *mode, const char *state)
{
    output << indent << iconName << ".addPixmap(" << call << ", QIcon"
        << language::qualifier << mode << ", QIcon" << language::qualifier
        << state << ')' << language::eol;
}

void WriteInitialization::writePixmapFunctionIcon(QTextStream &output,
                                                  const QString &iconName,
                                                  const QString &indent,
                                                  const DomResourceIcon *i) const
{
    if (i->hasElementNormalOff()) {
        writeIconAddPixmap(output, indent,  iconName,
                           pixCall(QLatin1String("QPixmap"), i->elementNormalOff()->text()),
                           "Normal", "Off");
    }
    if (i->hasElementNormalOn()) {
        writeIconAddPixmap(output, indent,  iconName,
                           pixCall(QLatin1String("QPixmap"), i->elementNormalOn()->text()),
                           "Normal", "On");
    }
    if (i->hasElementDisabledOff()) {
        writeIconAddPixmap(output, indent,  iconName,
                           pixCall(QLatin1String("QPixmap"), i->elementDisabledOff()->text()),
                           "Disabled", "Off");
    }
    if (i->hasElementDisabledOn()) {
        writeIconAddPixmap(output, indent,  iconName,
                           pixCall(QLatin1String("QPixmap"), i->elementDisabledOn()->text()),
                           "Disabled", "On");
    }
    if (i->hasElementActiveOff()) {
        writeIconAddPixmap(output, indent,  iconName,
                           pixCall(QLatin1String("QPixmap"), i->elementActiveOff()->text()),
                           "Active", "Off");
    }
    if (i->hasElementActiveOn()) {
        writeIconAddPixmap(output, indent,  iconName,
                           pixCall(QLatin1String("QPixmap"), i->elementActiveOn()->text()),
                           "Active", "On");
    }
    if (i->hasElementSelectedOff()) {
        writeIconAddPixmap(output, indent,  iconName,
                           pixCall(QLatin1String("QPixmap"), i->elementSelectedOff()->text()),
                           "Selected", "Off");
    }
    if (i->hasElementSelectedOn()) {
        writeIconAddPixmap(output, indent,  iconName,
                           pixCall(QLatin1String("QPixmap"), i->elementSelectedOn()->text()),
                           "Selected", "On");
    }
}

QString WriteInitialization::writeIconProperties(const DomResourceIcon *i)
{
    // check cache
    const IconHandle iconHandle(i);
    const IconPropertiesNameMap::const_iterator it = m_iconPropertiesNameMap.constFind(iconHandle);
    if (it != m_iconPropertiesNameMap.constEnd())
        return it.value();

    // insert with new name
    const QString iconName = m_driver->unique(QLatin1String("icon"));
    m_iconPropertiesNameMap.insert(IconHandle(i), iconName);

    const bool isCpp = language::language() == Language::Cpp;

    if (Q_UNLIKELY(!isIconFormat44(i))) { // pre-4.4 legacy
        m_output <<  m_indent;
        if (isCpp)
            m_output << "const QIcon ";
        m_output << iconName << " = " << pixCall(QLatin1String("QIcon"), i->text())
            << language::eol;
        return iconName;
    }

    // 4.4 onwards
    if (i->attributeTheme().isEmpty()) {
        // No theme: Write resource icon as is
        m_output << m_indent << language::stackVariable("QIcon", iconName)
            << language::eol;
        if (m_uic->pixmapFunction().isEmpty())
            writeResourceIcon(m_output, iconName, m_indent, i);
        else
            writePixmapFunctionIcon(m_output, iconName, m_indent, i);
        return iconName;
    }

    // Theme: Generate code to check the theme and default to resource
    if (iconHasStatePixmaps(i)) {
        // Theme + default state pixmaps:
        // Generate code to check the theme and default to state pixmaps
        m_output << m_indent << language::stackVariable("QIcon", iconName) << language::eol;
        const char themeNameStringVariableC[] = "iconThemeName";
        // Store theme name in a variable
        m_output << m_indent;
        if (m_firstThemeIcon) { // Declare variable string
            if (isCpp)
                m_output << "QString ";
            m_firstThemeIcon = false;
        }
        m_output << themeNameStringVariableC << " = "
            << language::qstring(i->attributeTheme()) << language::eol;
        m_output << m_indent << "if ";
        if (isCpp)
            m_output << '(';
        m_output << "QIcon" << language::qualifier << "hasThemeIcon("
            << themeNameStringVariableC << ')' << (isCpp ? ") {" : ":") << '\n'
            << m_dindent << iconName << " = QIcon" << language::qualifier << "fromTheme("
            << themeNameStringVariableC << ')' << language::eol
            << m_indent << (isCpp ? "} else {" : "else:") << '\n';
        if (m_uic->pixmapFunction().isEmpty())
            writeResourceIcon(m_output, iconName, m_dindent, i);
        else
            writePixmapFunctionIcon(m_output, iconName, m_dindent, i);
        m_output << m_indent;
        if (isCpp)
            m_output << '}';
        m_output  << '\n';
        return iconName;
    }

    // Theme, but no state pixmaps: Construct from theme directly.
    m_output << m_indent
        << language::stackVariableWithInitParameters("QIcon", iconName)
        << "QIcon" << language::qualifier << "fromTheme("
        << language::qstring(i->attributeTheme()) << "))"
        << language::eol;
    return iconName;
}

QString WriteInitialization::domColor2QString(const DomColor *c)
{
    if (c->hasAttributeAlpha())
        return QString::fromLatin1("QColor(%1, %2, %3, %4)")
            .arg(c->elementRed())
            .arg(c->elementGreen())
            .arg(c->elementBlue())
            .arg(c->attributeAlpha());
    return QString::fromLatin1("QColor(%1, %2, %3)")
        .arg(c->elementRed())
        .arg(c->elementGreen())
        .arg(c->elementBlue());
}

static inline QVersionNumber colorRoleVersionAdded(const QString &roleName)
{
    if (roleName == QLatin1String("PlaceholderText"))
        return QVersionNumber(5, 12, 0);
    return QVersionNumber();
}

void WriteInitialization::writeColorGroup(DomColorGroup *colorGroup, const QString &group, const QString &paletteName)
{
    if (!colorGroup)
        return;

    // old format
    const auto &colors = colorGroup->elementColor();
    for (int i=0; i<colors.size(); ++i) {
        const DomColor *color = colors.at(i);

        m_output << m_indent << paletteName << ".setColor(" << group
            << ", QPalette" << language::qualifier << language::paletteColorRole(i)
            << ", " << domColor2QString(color)
            << ")" << language::eol;
    }

    // new format
    const auto &colorRoles = colorGroup->elementColorRole();
    for (const DomColorRole *colorRole : colorRoles) {
        if (colorRole->hasAttributeRole()) {
            const QString roleName = colorRole->attributeRole();
            const QVersionNumber versionAdded = colorRoleVersionAdded(roleName);
            const QString brushName = writeBrushInitialization(colorRole->elementBrush());
            if (!versionAdded.isNull()) {
                m_output << "#if QT_VERSION >= QT_VERSION_CHECK("
                    << versionAdded.majorVersion() << ", " << versionAdded.minorVersion()
                    << ", " << versionAdded.microVersion() << ")\n";
            }
            m_output << m_indent << paletteName << ".setBrush("
                << language::enumValue(group) << ", "
                << "QPalette" << language::qualifier << roleName
                << ", " << brushName << ")" << language::eol;
            if (!versionAdded.isNull())
                m_output << "#endif\n";
        }
    }
}

// Write initialization for brush unless it is found in the cache. Returns the name to use
// in an expression.
QString WriteInitialization::writeBrushInitialization(const DomBrush *brush)
{
    // Simple solid, colored  brushes are cached
    const bool solidColoredBrush = !brush->hasAttributeBrushStyle() || brush->attributeBrushStyle() == QLatin1String("SolidPattern");
    uint rgb = 0;
    if (solidColoredBrush) {
        if (const DomColor *color = brush->elementColor()) {
            rgb = ((color->elementRed() & 0xFF) << 24) |
                  ((color->elementGreen() & 0xFF) << 16) |
                  ((color->elementBlue() & 0xFF) << 8) |
                  ((color->attributeAlpha() & 0xFF));
            const ColorBrushHash::const_iterator cit = m_colorBrushHash.constFind(rgb);
            if (cit != m_colorBrushHash.constEnd())
                return cit.value();
        }
    }
    // Create and enter into cache if simple
    const QString brushName = m_driver->unique(QLatin1String("brush"));
    writeBrush(brush, brushName);
    if (solidColoredBrush)
        m_colorBrushHash.insert(rgb, brushName);
    return brushName;
}

void WriteInitialization::writeBrush(const DomBrush *brush, const QString &brushName)
{
    QString style = QLatin1String("SolidPattern");
    if (brush->hasAttributeBrushStyle())
        style = brush->attributeBrushStyle();

    if (style == QLatin1String("LinearGradientPattern") ||
            style == QLatin1String("RadialGradientPattern") ||
            style == QLatin1String("ConicalGradientPattern")) {
        const DomGradient *gradient = brush->elementGradient();
        const QString gradientType = gradient->attributeType();
        const QString gradientName = m_driver->unique(QLatin1String("gradient"));
        if (gradientType == QLatin1String("LinearGradient")) {
            m_output << m_indent
                << language::stackVariableWithInitParameters("QLinearGradient", gradientName)
                << gradient->attributeStartX()
                << ", " << gradient->attributeStartY()
                << ", " << gradient->attributeEndX()
                << ", " << gradient->attributeEndY() << ')' << language::eol;
        } else if (gradientType == QLatin1String("RadialGradient")) {
            m_output << m_indent
                << language::stackVariableWithInitParameters("QRadialGradient", gradientName)
                << gradient->attributeCentralX()
                << ", " << gradient->attributeCentralY()
                << ", " << gradient->attributeRadius()
                << ", " << gradient->attributeFocalX()
                << ", " << gradient->attributeFocalY() << ')' << language::eol;
        } else if (gradientType == QLatin1String("ConicalGradient")) {
            m_output << m_indent
                << language::stackVariableWithInitParameters("QConicalGradient", gradientName)
                << gradient->attributeCentralX()
                << ", " << gradient->attributeCentralY()
                << ", " << gradient->attributeAngle() << ')' << language::eol;
        }

        m_output << m_indent << gradientName << ".setSpread(QGradient"
            << language::qualifier << gradient->attributeSpread()
            << ')' << language::eol;

        if (gradient->hasAttributeCoordinateMode()) {
            m_output << m_indent << gradientName << ".setCoordinateMode(QGradient"
                << language::qualifier << gradient->attributeCoordinateMode()
                << ')' << language::eol;
        }

       const auto &stops = gradient->elementGradientStop();
        for (const DomGradientStop *stop : stops) {
            const DomColor *color = stop->elementColor();
            m_output << m_indent << gradientName << ".setColorAt("
                << stop->attributePosition() << ", "
                << domColor2QString(color) << ')' << language::eol;
        }
        m_output << m_indent
            << language::stackVariableWithInitParameters("QBrush", brushName)
            << gradientName << ')' << language::eol;
    } else if (style == QLatin1String("TexturePattern")) {
        const DomProperty *property = brush->elementTexture();
        const QString iconValue = iconCall(property);

        m_output << m_indent
            << language::stackVariableWithInitParameters("QBrush", brushName)
            << iconValue << ')' << language::eol;
    } else {
        const DomColor *color = brush->elementColor();
        m_output << m_indent
            << language::stackVariableWithInitParameters("QBrush", brushName)
            << domColor2QString(color) << ')' << language::eol;

        m_output << m_indent << brushName << ".setStyle("
            << language::qtQualifier << style << ')' << language::eol;
    }
}

void WriteInitialization::acceptCustomWidget(DomCustomWidget *node)
{
    Q_UNUSED(node);
}

void WriteInitialization::acceptCustomWidgets(DomCustomWidgets *node)
{
    Q_UNUSED(node);
}

void WriteInitialization::acceptTabStops(DomTabStops *tabStops)
{
    QString lastName;

    const QStringList l = tabStops->elementTabStop();
    for (int i=0; i<l.size(); ++i) {
        const QString name = m_driver->widgetVariableName(l.at(i));

        if (name.isEmpty()) {
            fprintf(stderr, "%s: Warning: Tab-stop assignment: '%s' is not a valid widget.\n",
                    qPrintable(m_option.messagePrefix()), qPrintable(l.at(i)));
            continue;
        }

        if (i == 0) {
            lastName = name;
            continue;
        }
        if (name.isEmpty() || lastName.isEmpty())
            continue;

        m_output << m_indent << "QWidget" << language::qualifier << "setTabOrder("
            << lastName << ", " << name << ')' << language::eol;

        lastName = name;
    }
}

QString WriteInitialization::iconCall(const DomProperty *icon)
{
    if (icon->kind() == DomProperty::IconSet)
        return writeIconProperties(icon->elementIconSet());
    return pixCall(icon);
}

QString WriteInitialization::pixCall(const DomProperty *p) const
{
    QString type, s;
    switch (p->kind()) {
    case DomProperty::IconSet:
        type = QLatin1String("QIcon");
        s = p->elementIconSet()->text();
        break;
    case DomProperty::Pixmap:
        type = QLatin1String("QPixmap");
        s = p->elementPixmap()->text();
        break;
    default:
        qWarning("%s: Warning: Unknown icon format encountered. The ui-file was generated with a too-recent version of Designer.",
                 qPrintable(m_option.messagePrefix()));
        return QLatin1String("QIcon()");
        break;
    }
    return pixCall(type, s);
}

QString WriteInitialization::pixCall(const QString &t, const QString &text) const
{
    QString type = t;
    if (text.isEmpty()) {
        type += QLatin1String("()");
        return type;
    }

    QTextStream str(&type);
    str << '(';
    QString pixFunc = m_uic->pixmapFunction();
    if (pixFunc.isEmpty())
        str << language::qstring(text, m_dindent);
    else
        str << pixFunc << '(' << language::charliteral(text, m_dindent) << ')';
    str << ')';
    return type;
}

void WriteInitialization::initializeComboBox(DomWidget *w)
{
    const QString varName = m_driver->findOrInsertWidget(w);

    const auto &items = w->elementItem();

    if (items.isEmpty())
        return;

    for (int i = 0; i < items.size(); ++i) {
        const DomItem *item = items.at(i);
        const DomPropertyMap properties = propertyMap(item->elementProperty());
        const DomProperty *text = properties.value(QLatin1String("text"));
        const DomProperty *icon = properties.value(QLatin1String("icon"));

        QString iconValue;
        if (icon)
            iconValue = iconCall(icon);

        m_output << m_indent << varName << language::derefPointer << "addItem(";
        if (icon)
            m_output << iconValue << ", ";

        if (needsTranslation(text->elementString())) {
            m_output << language::emptyString << ')' << language::eol;
            m_refreshOut << m_indent << varName << language::derefPointer
                << "setItemText(" << i << ", " << trCall(text->elementString())
                << ')' << language::eol;
        } else {
            m_output << noTrCall(text->elementString()) << ")" << language::eol;
        }
    }
    m_refreshOut << "\n";
}

QString WriteInitialization::disableSorting(DomWidget *w, const QString &varName)
{
    // turn off sortingEnabled to force programmatic item order (setItem())
    QString tempName;
    if (!w->elementItem().isEmpty()) {
        tempName = m_driver->unique(QLatin1String("__sortingEnabled"));
        m_refreshOut << "\n";
        m_refreshOut << m_indent;
        if (language::language() == Language::Cpp)
            m_refreshOut << "const bool ";
        m_refreshOut << tempName << " = " << varName << language::derefPointer
            << "isSortingEnabled()" << language::eol
            << m_indent << varName << language::derefPointer
            << "setSortingEnabled(" << language::boolValue(false) << ')' << language::eol;
    }
    return tempName;
}

void WriteInitialization::enableSorting(DomWidget *w, const QString &varName, const QString &tempName)
{
    if (!w->elementItem().isEmpty()) {
        m_refreshOut << m_indent << varName << language::derefPointer
            << "setSortingEnabled(" << tempName << ')' << language::eol << '\n';
    }
}

/*
 * Initializers are just strings containing the function call and need to be prepended
 * the line indentation and the object they are supposed to initialize.
 * String initializers come with a preprocessor conditional (ifdef), so the code
 * compiles with QT_NO_xxx. A null pointer means no conditional. String initializers
 * are written to the retranslateUi() function, others to setupUi().
 */


/*!
    Create non-string inititializer.
    \param value the value to initialize the attribute with. May be empty, in which case
        the initializer is omitted.
    See above for other parameters.
*/
void WriteInitialization::addInitializer(Item *item,
        const QString &name, int column, const QString &value, const QString &directive, bool translatable) const
{
    if (!value.isEmpty()) {
        QString setter;
        QTextStream str(&setter);
        str << language::derefPointer << "set" << name.at(0).toUpper() << QStringView{name}.mid(1) << '(';
        if (column >= 0)
            str << column << ", ";
        str << value << ");";
        item->addSetter(setter, directive, translatable);
    }
}

/*!
    Create string inititializer.
    \param initializers in/out list of inializers
    \param properties map property name -> property to extract data from
    \param name the property to extract
    \param col the item column to generate the initializer for. This is relevant for
        tree widgets only. If it is -1, no column index will be generated.
    \param ifdef preprocessor symbol for disabling compilation of this initializer
*/
void WriteInitialization::addStringInitializer(Item *item,
        const DomPropertyMap &properties, const QString &name, int column, const QString &directive) const
{
    if (const DomProperty *p = properties.value(name)) {
        DomString *str = p->elementString();
        QString text = toString(str);
        if (!text.isEmpty()) {
            bool translatable = needsTranslation(str);
            QString value = autoTrCall(str);
            addInitializer(item, name, column, value, directive, translatable);
        }
    }
}

void WriteInitialization::addBrushInitializer(Item *item,
        const DomPropertyMap &properties, const QString &name, int column)
{
    if (const DomProperty *p = properties.value(name)) {
        if (p->elementBrush())
            addInitializer(item, name, column, writeBrushInitialization(p->elementBrush()));
        else if (p->elementColor())
            addInitializer(item, name, column, domColor2QString(p->elementColor()));
    }
}

/*!
    Create inititializer for a flag value in the Qt namespace.
    If the named property is not in the map, the initializer is omitted.
*/
void WriteInitialization::addQtFlagsInitializer(Item *item,
        const DomPropertyMap &properties, const QString &name, int column) const
{
    if (const DomProperty *p = properties.value(name)) {
        const QString orOperator = QLatin1Char('|') + language::qtQualifier;
        QString v = p->elementSet();
        if (!v.isEmpty()) {
            v.replace(QLatin1Char('|'), orOperator);
            addInitializer(item, name, column, language::qtQualifier + v);
        }
    }
}

/*!
    Create inititializer for an enum value in the Qt namespace.
    If the named property is not in the map, the initializer is omitted.
*/
void WriteInitialization::addQtEnumInitializer(Item *item,
        const DomPropertyMap &properties, const QString &name, int column) const
{
    if (const DomProperty *p = properties.value(name)) {
        QString v = p->elementEnum();
        if (!v.isEmpty())
            addInitializer(item, name, column, language::qtQualifier + v);
    }
}

/*!
    Create inititializers for all common properties that may be bound to a column.
*/
void WriteInitialization::addCommonInitializers(Item *item,
        const DomPropertyMap &properties, int column)
{
    if (const DomProperty *icon = properties.value(QLatin1String("icon")))
        addInitializer(item, QLatin1String("icon"), column, iconCall(icon));
    addBrushInitializer(item, properties, QLatin1String("foreground"), column);
    addBrushInitializer(item, properties, QLatin1String("background"), column);
    if (const DomProperty *font = properties.value(QLatin1String("font")))
        addInitializer(item, QLatin1String("font"), column, writeFontProperties(font->elementFont()));
    addQtFlagsInitializer(item, properties, QLatin1String("textAlignment"), column);
    addQtEnumInitializer(item, properties, QLatin1String("checkState"), column);
    addStringInitializer(item, properties, QLatin1String("text"), column);
    addStringInitializer(item, properties, QLatin1String("toolTip"), column,
                         toolTipConfigKey());
    addStringInitializer(item, properties, QLatin1String("whatsThis"), column,
                         whatsThisConfigKey());
    addStringInitializer(item, properties, QLatin1String("statusTip"), column,
                         statusTipConfigKey());
}

void WriteInitialization::initializeListWidget(DomWidget *w)
{
    const QString varName = m_driver->findOrInsertWidget(w);

    const auto &items = w->elementItem();

    if (items.isEmpty())
        return;

    QString tempName = disableSorting(w, varName);
    // items
    // TODO: the generated code should be data-driven to reduce its size
    for (int i = 0; i < items.size(); ++i) {
        const DomItem *domItem = items.at(i);

        const DomPropertyMap properties = propertyMap(domItem->elementProperty());

        Item item(QLatin1String("QListWidgetItem"), m_indent, m_output, m_refreshOut, m_driver);
        addQtFlagsInitializer(&item, properties, QLatin1String("flags"));
        addCommonInitializers(&item, properties);

        item.writeSetupUi(varName);
        QString parentPath;
        QTextStream(&parentPath) << varName << language::derefPointer << "item(" << i << ')';
        item.writeRetranslateUi(parentPath);
    }
    enableSorting(w, varName, tempName);
}

void WriteInitialization::initializeTreeWidget(DomWidget *w)
{
    const QString varName = m_driver->findOrInsertWidget(w);

    // columns
    Item item(QLatin1String("QTreeWidgetItem"), m_indent, m_output, m_refreshOut, m_driver);

    const auto &columns = w->elementColumn();
    for (int i = 0; i < columns.size(); ++i) {
        const DomColumn *column = columns.at(i);

        const DomPropertyMap properties = propertyMap(column->elementProperty());
        addCommonInitializers(&item, properties, i);

        if (const DomProperty *p = properties.value(QLatin1String("text"))) {
            DomString *str = p->elementString();
            if (str && str->text().isEmpty()) {
                m_output << m_indent << varName << language::derefPointer
                    << "headerItem()" << language::derefPointer << "setText("
                    << i << ", " << language::emptyString << ')' << language::eol;
            }
        }
    }
    const QString itemName = item.writeSetupUi(QString(), Item::DontConstruct);
    item.writeRetranslateUi(varName + language::derefPointer + QLatin1String("headerItem()"));
    if (!itemName.isNull()) {
        m_output << m_indent << varName << language::derefPointer
            << "setHeaderItem(" << itemName << ')' << language::eol;
    }

    if (w->elementItem().empty())
        return;

    QString tempName = disableSorting(w, varName);

    const auto items = initializeTreeWidgetItems(w->elementItem());
    for (int i = 0; i < items.count(); i++) {
        Item *itm = items[i];
        itm->writeSetupUi(varName);
        QString parentPath;
        QTextStream(&parentPath) << varName << language::derefPointer << "topLevelItem(" << i << ')';
        itm->writeRetranslateUi(parentPath);
        delete itm;
    }

    enableSorting(w, varName, tempName);
}

/*!
    Create and write out initializers for tree widget items.
    This function makes sure that only needed items are fetched (subject to preprocessor
    conditionals), that each item is fetched from its parent widget/item exactly once
    and that no temporary variables are created for items that are needed only once. As
    fetches are built top-down from the root, but determining how often and under which
    conditions an item is needed needs to be done bottom-up, the whole process makes
    two passes, storing the intermediate result in a recursive StringInitializerListMap.
*/
WriteInitialization::Items WriteInitialization::initializeTreeWidgetItems(const QVector<DomItem *> &domItems)
{
    // items
    Items items;
    const int numDomItems = domItems.size();
    items.reserve(numDomItems);

    for (int i = 0; i < numDomItems; ++i) {
        const DomItem *domItem = domItems.at(i);

        Item *item = new Item(QLatin1String("QTreeWidgetItem"), m_indent, m_output, m_refreshOut, m_driver);
        items << item;

        QHash<QString, DomProperty *> map;

        int col = -1;
        const DomPropertyList properties = domItem->elementProperty();
        for (DomProperty *p : properties) {
             if (p->attributeName() == QLatin1String("text")) {
                if (!map.isEmpty()) {
                    addCommonInitializers(item, map, col);
                    map.clear();
                }
                col++;
            }
            map.insert(p->attributeName(), p);
        }
        addCommonInitializers(item, map, col);
        // AbstractFromBuilder saves flags last, so they always end up in the last column's map.
        addQtFlagsInitializer(item, map, QLatin1String("flags"));

        const auto subItems = initializeTreeWidgetItems(domItem->elementItem());
        for (Item *subItem : subItems)
            item->addChild(subItem);
    }
    return items;
}

void WriteInitialization::initializeTableWidget(DomWidget *w)
{
    const QString varName = m_driver->findOrInsertWidget(w);

    // columns
    const auto &columns = w->elementColumn();

    if (!columns.empty()) {
        m_output << m_indent << "if (" << varName << language::derefPointer
            << "columnCount() < " << columns.size() << ')';
        if (language::language() == Language::Python)
            m_output << ':';
        m_output << '\n' << m_dindent << varName << language::derefPointer << "setColumnCount("
            << columns.size() << ')' << language::eol;
    }

    for (int i = 0; i < columns.size(); ++i) {
        const DomColumn *column = columns.at(i);
        if (!column->elementProperty().isEmpty()) {
            const DomPropertyMap properties = propertyMap(column->elementProperty());

            Item item(QLatin1String("QTableWidgetItem"), m_indent, m_output, m_refreshOut, m_driver);
            addCommonInitializers(&item, properties);

            QString itemName = item.writeSetupUi(QString(), Item::ConstructItemAndVariable);
            QString parentPath;
            QTextStream(&parentPath) << varName << language::derefPointer
                << "horizontalHeaderItem(" << i << ')';
            item.writeRetranslateUi(parentPath);
            m_output << m_indent << varName << language::derefPointer << "setHorizontalHeaderItem("
                << i << ", " << itemName << ')' << language::eol;
        }
    }

    // rows
    const auto &rows = w->elementRow();

    if (!rows.isEmpty()) {
        m_output << m_indent << "if (" << varName << language::derefPointer
            << "rowCount() < " << rows.size() << ')';
        if (language::language() == Language::Python)
            m_output << ':';
        m_output << '\n' << m_dindent << varName << language::derefPointer << "setRowCount("
            << rows.size() << ')' << language::eol;
    }

    for (int i = 0; i < rows.size(); ++i) {
        const DomRow *row = rows.at(i);
        if (!row->elementProperty().isEmpty()) {
            const DomPropertyMap properties = propertyMap(row->elementProperty());

            Item item(QLatin1String("QTableWidgetItem"), m_indent, m_output, m_refreshOut, m_driver);
            addCommonInitializers(&item, properties);

            QString itemName = item.writeSetupUi(QString(), Item::ConstructItemAndVariable);
            QString parentPath;
            QTextStream(&parentPath) << varName << language::derefPointer << "verticalHeaderItem(" << i << ')';
            item.writeRetranslateUi(parentPath);
            m_output << m_indent << varName << language::derefPointer << "setVerticalHeaderItem("
                << i << ", " << itemName << ')' << language::eol;
        }
    }

    // items
    QString tempName = disableSorting(w, varName);

    const auto &items = w->elementItem();

    for (const DomItem *cell : items) {
        if (cell->hasAttributeRow() && cell->hasAttributeColumn() && !cell->elementProperty().isEmpty()) {
            const int r = cell->attributeRow();
            const int c = cell->attributeColumn();
            const DomPropertyMap properties = propertyMap(cell->elementProperty());

            Item item(QLatin1String("QTableWidgetItem"), m_indent, m_output, m_refreshOut, m_driver);
            addQtFlagsInitializer(&item, properties, QLatin1String("flags"));
            addCommonInitializers(&item, properties);

            QString itemName = item.writeSetupUi(QString(), Item::ConstructItemAndVariable);
            QString parentPath;
            QTextStream(&parentPath) << varName << language::derefPointer << "item(" << r
                << ", " << c << ')';
            item.writeRetranslateUi(parentPath);
            m_output << m_indent << varName << language::derefPointer << "setItem("
                << r << ", " << c << ", " << itemName << ')' << language::eol;
        }
    }
    enableSorting(w, varName, tempName);
}

QString WriteInitialization::trCall(const QString &str, const QString &commentHint, const QString &id) const
{
    if (str.isEmpty())
        return language::emptyString;

    QString result;
    QTextStream ts(&result);

    const bool idBasedTranslations = m_driver->useIdBasedTranslations();
    if (m_option.translateFunction.isEmpty()) {
        if (idBasedTranslations || m_option.idBased) {
            ts << "qtTrId(";
        } else {
            ts << "QCoreApplication" << language::qualifier << "translate("
                << '"' << m_generatedClass << "\", ";
        }
    } else {
        ts << m_option.translateFunction << '(';
    }

    ts << language::charliteral(idBasedTranslations ? id : str, m_dindent);

    if (!idBasedTranslations && !m_option.idBased) {
        ts << ", ";
        if (commentHint.isEmpty())
            ts << language::nullPtr;
        else
            ts << language::charliteral(commentHint, m_dindent);
    }

    ts << ')';
    return result;
}

void WriteInitialization::initializeMenu(DomWidget *w, const QString &/*parentWidget*/)
{
    const QString menuName = m_driver->findOrInsertWidget(w);
    const QString menuAction = menuName + QLatin1String("Action");

    const DomAction *action = m_driver->actionByName(menuAction);
    if (action && action->hasAttributeMenu()) {
        m_output << m_indent << menuAction << " = " << menuName
            << language::derefPointer << "menuAction()" << language::eol;
    }
}

QString WriteInitialization::trCall(DomString *str, const QString &defaultString) const
{
    QString value = defaultString;
    QString comment;
    QString id;
    if (str) {
        value = toString(str);
        comment = str->attributeComment();
        id = str->attributeId();
    }
    return trCall(value, comment, id);
}

QString WriteInitialization::noTrCall(DomString *str, const QString &defaultString) const
{
    QString value = defaultString;
    if (!str && defaultString.isEmpty())
        return QString();
    if (str)
        value = str->text();
    QString ret;
    QTextStream ts(&ret);
    ts << language::qstring(value, m_dindent);
    return ret;
}

QString WriteInitialization::autoTrCall(DomString *str, const QString &defaultString) const
{
    if ((!str && !defaultString.isEmpty()) || needsTranslation(str))
        return trCall(str, defaultString);
    return noTrCall(str, defaultString);
}

QTextStream &WriteInitialization::autoTrOutput(const DomProperty *property)
{
    if (const DomString *str = property->elementString())
        return autoTrOutput(str);
    if (const DomStringList *list = property->elementStringList())
        if (needsTranslation(list))
            return m_refreshOut;
    return m_output;
}

QTextStream &WriteInitialization::autoTrOutput(const DomString *str, const QString &defaultString)
{
    if ((!str && !defaultString.isEmpty()) || needsTranslation(str))
        return m_refreshOut;
    return m_output;
}

WriteInitialization::Declaration WriteInitialization::findDeclaration(const QString &name)
{
    if (const DomWidget *widget = m_driver->widgetByName(name))
        return {m_driver->findOrInsertWidget(widget), widget->attributeClass()};
    if (const DomAction *action = m_driver->actionByName(name))
        return {m_driver->findOrInsertAction(action), QStringLiteral("QAction")};
    if (const DomButtonGroup *group = m_driver->findButtonGroup(name))
         return {m_driver->findOrInsertButtonGroup(group), QStringLiteral("QButtonGroup")};
    return {};
}

bool WriteInitialization::isCustomWidget(const QString &className) const
{
    return m_uic->customWidgetsInfo()->customWidget(className) != nullptr;
}

ConnectionSyntax WriteInitialization::connectionSyntax(const language::SignalSlot &sender,
                                                       const language::SignalSlot &receiver) const
{
    if (m_option.forceMemberFnPtrConnectionSyntax)
        return ConnectionSyntax::MemberFunctionPtr;
    if (m_option.forceStringConnectionSyntax)
        return ConnectionSyntax::StringBased;
    // Auto mode: Use Qt 5 connection syntax for Qt classes and parameterless
    // connections. QAxWidget is special though since it has a fake Meta object.
    static const QStringList requiresStringSyntax{QStringLiteral("QAxWidget")};
    if (requiresStringSyntax.contains(sender.className)
        || requiresStringSyntax.contains(receiver.className)) {
        return ConnectionSyntax::StringBased;
    }

    if ((sender.name == m_mainFormVarName && m_customSignals.contains(sender.signature))
         || (receiver.name == m_mainFormVarName && m_customSlots.contains(receiver.signature))) {
        return ConnectionSyntax::StringBased;
    }

    return sender.signature.endsWith(QLatin1String("()"))
        || (!isCustomWidget(sender.className) && !isCustomWidget(receiver.className))
        ? ConnectionSyntax::MemberFunctionPtr : ConnectionSyntax::StringBased;
}

void WriteInitialization::acceptConnection(DomConnection *connection)
{
    const QString senderName = connection->elementSender();
    const QString receiverName = connection->elementReceiver();

    const auto senderDecl = findDeclaration(senderName);
    const auto receiverDecl = findDeclaration(receiverName);

    if (senderDecl.name.isEmpty() || receiverDecl.name.isEmpty()) {
        QString message;
        QTextStream(&message) << m_option.messagePrefix()
            << ": Warning: Invalid signal/slot connection: \""
            << senderName << "\" -> \"" << receiverName << "\".";
        fprintf(stderr, "%s\n", qPrintable(message));
        return;
    }
    const QString senderSignature = connection->elementSignal();
    language::SignalSlot theSignal{senderDecl.name, senderSignature,
                                   senderDecl.className};
    language::SignalSlot theSlot{receiverDecl.name, connection->elementSlot(),
                                 receiverDecl.className};

    m_output << m_indent;
    language::formatConnection(m_output, theSignal, theSlot,
                               connectionSyntax(theSignal, theSlot));
    m_output << language::eol;
}

static void generateMultiDirectiveBegin(QTextStream &outputStream, const QSet<QString> &directives)
{
    if (directives.isEmpty())
        return;

    if (directives.size() == 1) {
        outputStream << language::openQtConfig(*directives.cbegin());
        return;
    }

    auto list = directives.values();
    // sort (always generate in the same order):
    std::sort(list.begin(), list.end());

    outputStream << "#if " << language::qtConfig(list.constFirst());
    for (int i = 1, size = list.size(); i < size; ++i)
        outputStream << " || " << language::qtConfig(list.at(i));
    outputStream << Qt::endl;
}

static void generateMultiDirectiveEnd(QTextStream &outputStream, const QSet<QString> &directives)
{
    if (directives.isEmpty())
        return;

    outputStream << "#endif" << Qt::endl;
}

WriteInitialization::Item::Item(const QString &itemClassName, const QString &indent, QTextStream &setupUiStream, QTextStream &retranslateUiStream, Driver *driver)
    :
    m_itemClassName(itemClassName),
    m_indent(indent),
    m_setupUiStream(setupUiStream),
    m_retranslateUiStream(retranslateUiStream),
    m_driver(driver)
{

}

WriteInitialization::Item::~Item()
{
    qDeleteAll(m_children);
}

QString WriteInitialization::Item::writeSetupUi(const QString &parent, Item::EmptyItemPolicy emptyItemPolicy)
{
    if (emptyItemPolicy == Item::DontConstruct && m_setupUiData.policy == ItemData::DontGenerate)
        return QString();

    bool generateMultiDirective = false;
    if (emptyItemPolicy == Item::ConstructItemOnly && m_children.isEmpty()) {
        if (m_setupUiData.policy == ItemData::DontGenerate) {
            m_setupUiStream << m_indent << language::operatorNew << m_itemClassName
                << '(' << parent << ')' << language::eol;
            return QString();
        }
        if (m_setupUiData.policy == ItemData::GenerateWithMultiDirective)
            generateMultiDirective = true;
    }

    if (generateMultiDirective)
        generateMultiDirectiveBegin(m_setupUiStream, m_setupUiData.directives);

    const QString uniqueName = m_driver->unique(QLatin1String("__") + m_itemClassName.toLower());
    m_setupUiStream << m_indent;
    if (language::language() == Language::Cpp)
        m_setupUiStream << m_itemClassName << " *";
    m_setupUiStream << uniqueName
        << " = " << language::operatorNew << m_itemClassName << '(' << parent
        << ')' << language::eol;

    if (generateMultiDirective) {
        m_setupUiStream << "#else\n";
        m_setupUiStream << m_indent << language::operatorNew << m_itemClassName
            << '(' << parent << ')' << language::eol;
        generateMultiDirectiveEnd(m_setupUiStream, m_setupUiData.directives);
    }

    QMultiMap<QString, QString>::ConstIterator it = m_setupUiData.setters.constBegin();
    while (it != m_setupUiData.setters.constEnd()) {
        if (!it.key().isEmpty())
            m_setupUiStream << language::openQtConfig(it.key());
        m_setupUiStream << m_indent << uniqueName << it.value() << Qt::endl;
        if (!it.key().isEmpty())
            m_setupUiStream << language::closeQtConfig(it.key());
        ++it;
    }
    for (Item *child : qAsConst(m_children))
        child->writeSetupUi(uniqueName);
    return uniqueName;
}

void WriteInitialization::Item::writeRetranslateUi(const QString &parentPath)
{
    if (m_retranslateUiData.policy == ItemData::DontGenerate)
        return;

    if (m_retranslateUiData.policy == ItemData::GenerateWithMultiDirective)
        generateMultiDirectiveBegin(m_retranslateUiStream, m_retranslateUiData.directives);

    const QString uniqueName = m_driver->unique(QLatin1String("___") + m_itemClassName.toLower());
    m_retranslateUiStream << m_indent;
    if (language::language() == Language::Cpp)
        m_retranslateUiStream << m_itemClassName << " *";
    m_retranslateUiStream << uniqueName << " = " << parentPath << language::eol;

    if (m_retranslateUiData.policy == ItemData::GenerateWithMultiDirective)
        generateMultiDirectiveEnd(m_retranslateUiStream, m_retranslateUiData.directives);

    QString oldDirective;
    QMultiMap<QString, QString>::ConstIterator it = m_retranslateUiData.setters.constBegin();
    while (it != m_retranslateUiData.setters.constEnd()) {
        const QString newDirective = it.key();
        if (oldDirective != newDirective) {
            if (!oldDirective.isEmpty())
                m_retranslateUiStream << language::closeQtConfig(oldDirective);
            if (!newDirective.isEmpty())
                m_retranslateUiStream << language::openQtConfig(newDirective);
            oldDirective = newDirective;
        }
        m_retranslateUiStream << m_indent << uniqueName << it.value() << Qt::endl;
        ++it;
    }
    if (!oldDirective.isEmpty())
        m_retranslateUiStream << language::closeQtConfig(oldDirective);

    for (int i = 0; i < m_children.size(); i++) {
        QString method;
        QTextStream(&method) << uniqueName << language::derefPointer << "child(" << i << ')';
        m_children[i]->writeRetranslateUi(method);
    }
}

void WriteInitialization::Item::addSetter(const QString &setter, const QString &directive, bool translatable)
{
    const ItemData::TemporaryVariableGeneratorPolicy newPolicy = directive.isNull() ? ItemData::Generate : ItemData::GenerateWithMultiDirective;
    if (translatable) {
        m_retranslateUiData.setters.insert(directive, setter);
        if (ItemData::GenerateWithMultiDirective == newPolicy)
            m_retranslateUiData.directives << directive;
        if (m_retranslateUiData.policy < newPolicy)
            m_retranslateUiData.policy = newPolicy;
    } else {
        m_setupUiData.setters.insert(directive, setter);
        if (ItemData::GenerateWithMultiDirective == newPolicy)
            m_setupUiData.directives << directive;
        if (m_setupUiData.policy < newPolicy)
            m_setupUiData.policy = newPolicy;
    }
}

void WriteInitialization::Item::addChild(Item *child)
{
    m_children << child;
    child->m_parent = this;

    Item *c = child;
    Item *p = this;
    while (p) {
        p->m_setupUiData.directives |= c->m_setupUiData.directives;
        p->m_retranslateUiData.directives |= c->m_retranslateUiData.directives;
        if (p->m_setupUiData.policy < c->m_setupUiData.policy)
            p->m_setupUiData.policy = c->m_setupUiData.policy;
        if (p->m_retranslateUiData.policy < c->m_retranslateUiData.policy)
            p->m_retranslateUiData.policy = c->m_retranslateUiData.policy;
        c = p;
        p = p->m_parent;
    }
}


} // namespace CPP

QT_END_NAMESPACE
