/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "texttomodelmerger.h"
#include "abstractproperty.h"
#include "bindingproperty.h"
#include "filemanager/firstdefinitionfinder.h"
#include "filemanager/objectlengthcalculator.h"
#include "filemanager/qmlrefactoring.h"
#include "filemanager/qmlwarningdialog.h"
#include "nodeproperty.h"
#include "propertyparser.h"
#include "rewriterview.h"
#include "variantproperty.h"
#include "signalhandlerproperty.h"
#include "nodemetainfo.h"

#include <qmljs/qmljsevaluate.h>
#include <qmljs/qmljslink.h>
#include <qmljs/parser/qmljsast_p.h>
#include <qmljs/qmljscheck.h>
#include <qmljs/qmljsutils.h>
#include <qmljs/qmljsmodelmanagerinterface.h>
#include <qmljs/qmljsqrcparser.h>

#include <QSet>
#include <QDir>

using namespace LanguageUtils;
using namespace QmlJS;
using namespace QmlJS::AST;

namespace {

static inline QStringList supportedVersionsList()
{
    QStringList list;
    list << QLatin1String("1.0") << QLatin1String("1.1") << QLatin1String("2.0") << QLatin1String("2.1");
    return list;
}

static inline QStringList globalQtEnums()
{
    static QStringList list = QStringList() << QLatin1String("Horizontal") << QLatin1String("Vertical") << QLatin1String("AlignVCenter")
         << QLatin1String("AlignLeft") << QLatin1String("LeftToRight") << QLatin1String("RightToLeft");
    return list;
}

static inline bool supportedQtQuickVersion(const QString &version)
{
    static QStringList supportedVersions = supportedVersionsList();

    return supportedVersions.contains(version);
}

static inline QString stripQuotes(const QString &str)
{
    if ((str.startsWith(QLatin1Char('"')) && str.endsWith(QLatin1Char('"')))
            || (str.startsWith(QLatin1Char('\'')) && str.endsWith(QLatin1Char('\''))))
        return str.mid(1, str.length() - 2);

    return str;
}

static inline QString deEscape(const QString &value)
{
    QString result = value;

    result.replace(QLatin1String("\\\\"), QLatin1String("\\"));
    result.replace(QLatin1String("\\\""), QLatin1String("\""));
    result.replace(QLatin1String("\\t"), QLatin1String("\t"));
    result.replace(QLatin1String("\\r"), QLatin1String("\\\r"));
    result.replace(QLatin1String("\\n"), QLatin1String("\n"));

    return result;
}

static inline unsigned char convertHex(ushort c)
{
    if (c >= '0' && c <= '9')
        return (c - '0');
    else if (c >= 'a' && c <= 'f')
        return (c - 'a' + 10);
    else
        return (c - 'A' + 10);
}

static inline unsigned char convertHex(ushort c1, ushort c2)
{
    return ((convertHex(c1) << 4) + convertHex(c2));
}

QChar convertUnicode(ushort c1, ushort c2,
                             ushort c3, ushort c4)
{
    return QChar((convertHex(c3) << 4) + convertHex(c4),
                  (convertHex(c1) << 4) + convertHex(c2));
}

static inline bool isHexDigit(ushort c)
{
    return ((c >= '0' && c <= '9')
            || (c >= 'a' && c <= 'f')
            || (c >= 'A' && c <= 'F'));
}


static inline QString fixEscapedUnicodeChar(const QString &value) //convert "\u2939"
{
    if (value.count() == 6 && value.at(0) == '\\' && value.at(1) == 'u' &&
        isHexDigit(value.at(2).unicode()) && isHexDigit(value.at(3).unicode()) &&
        isHexDigit(value.at(4).unicode()) && isHexDigit(value.at(5).unicode())) {
            return convertUnicode(value.at(2).unicode(), value.at(3).unicode(), value.at(4).unicode(), value.at(5).unicode());
    }
    return value;
}

static inline bool isSignalPropertyName(const QString &signalName)
{
    // see QmlCompiler::isSignalPropertyName
    return signalName.length() >= 3 && signalName.startsWith(QLatin1String("on")) &&
           signalName.at(2).isLetter();
}

static inline QVariant cleverConvert(const QString &value)
{
    if (value == "true")
        return QVariant(true);
    if (value == "false")
        return QVariant(false);
    bool flag;
    int i = value.toInt(&flag);
    if (flag)
        return QVariant(i);
    double d = value.toDouble(&flag);
    if (flag)
        return QVariant(d);
    return QVariant(value);
}

static bool isLiteralValue(ExpressionNode *expr)
{
    if (cast<NumericLiteral*>(expr))
        return true;
    else if (cast<StringLiteral*>(expr))
        return true;
    else if (UnaryPlusExpression *plusExpr = cast<UnaryPlusExpression*>(expr))
        return isLiteralValue(plusExpr->expression);
    else if (UnaryMinusExpression *minusExpr = cast<UnaryMinusExpression*>(expr))
        return isLiteralValue(minusExpr->expression);
    else if (cast<TrueLiteral*>(expr))
        return true;
    else if (cast<FalseLiteral*>(expr))
        return true;
    else
        return false;
}

static bool isLiteralValue(Statement *stmt)
{
    ExpressionStatement *exprStmt = cast<ExpressionStatement *>(stmt);
    if (exprStmt)
        return isLiteralValue(exprStmt->expression);
    else
        return false;
}

static inline bool isLiteralValue(UiScriptBinding *script)
{
    if (!script || !script->statement)
        return false;

    return isLiteralValue(script->statement);
}

static inline int propertyType(const QString &typeName)
{
    if (typeName == QLatin1String("bool"))
        return QMetaType::type("bool");
    else if (typeName == QLatin1String("color"))
        return QMetaType::type("QColor");
    else if (typeName == QLatin1String("date"))
        return QMetaType::type("QDate");
    else if (typeName == QLatin1String("int"))
        return QMetaType::type("int");
    else if (typeName == QLatin1String("real"))
        return QMetaType::type("double");
    else if (typeName == QLatin1String("double"))
        return QMetaType::type("double");
    else if (typeName == QLatin1String("string"))
        return QMetaType::type("QString");
    else if (typeName == QLatin1String("url"))
        return QMetaType::type("QUrl");
    else if (typeName == QLatin1String("var") || typeName == QLatin1String("variant"))
        return QMetaType::type("QVariant");
    else
        return -1;
}

static inline QVariant convertDynamicPropertyValueToVariant(const QString &astValue,
                                                            const QString &astType)
{
    const QString cleanedValue = fixEscapedUnicodeChar(deEscape(stripQuotes(astValue.trimmed())));

    if (astType.isEmpty())
        return QString();

    const int type = propertyType(astType);
    if (type == QMetaType::type("QVariant")) {
        if (cleanedValue.isNull()) // Explicitly isNull, NOT isEmpty!
            return QVariant(static_cast<QVariant::Type>(type));
        else
            return QVariant(cleanedValue);
    } else {
        QVariant value = QVariant(cleanedValue);
        value.convert(static_cast<QVariant::Type>(type));
        return value;
    }
}

static bool isComponentType(const QmlDesigner::TypeName &type)
{
    return  type == "Component" || type == "Qt.Component" || type == "QtQuick.Component";
}

static bool isCustomParserType(const QString &type)
{
    return type == "QtQuick.VisualItemModel" || type == "Qt.VisualItemModel" ||
           type == "QtQuick.VisualDataModel" || type == "Qt.VisualDataModel" ||
           type == "QtQuick.ListModel" || type == "Qt.ListModel" ||
           type == "QtQuick.XmlListModel" || type == "Qt.XmlListModel";
}


static bool isPropertyChangesType(const QmlDesigner::TypeName &type)
{
    return  type == "PropertyChanges" || type == "QtQuick.PropertyChanges" || type == "Qt.PropertyChanges";
}

static bool isConnectionsType(const QmlDesigner::TypeName &type)
{
    return  type == "Connections" || type == "QtQuick.Connections" || type == "Qt.Connections";
}

static bool propertyIsComponentType(const QmlDesigner::NodeAbstractProperty &property, const QmlDesigner::TypeName &type, QmlDesigner::Model *model)
{
    if (model->metaInfo(type, -1, -1).isSubclassOf("QtQuick.Component", -1, -1) && !isComponentType(type))
        return false; //If the type is already a subclass of Component keep it

    return property.parentModelNode().isValid() &&
            isComponentType(property.parentModelNode().metaInfo().propertyTypeName(property.name()));
}

static inline QString extractComponentFromQml(const QString &source)
{
    if (source.isEmpty())
        return QString();

    QString result;
    if (source.contains("Component")) { //explicit component
        QmlDesigner::FirstDefinitionFinder firstDefinitionFinder(source);
        int offset = firstDefinitionFinder(0);
        if (offset < 0)
            return QString(); //No object definition found
        QmlDesigner::ObjectLengthCalculator objectLengthCalculator;
        unsigned length;
        if (objectLengthCalculator(source, offset, length))
            result = source.mid(offset, length);
        else
            result = source;
    } else {
        result = source; //implicit component
    }
    return result;
}

} // anonymous namespace

namespace QmlDesigner {
namespace Internal {

class ReadingContext
{
public:
    ReadingContext(const Snapshot &snapshot, const Document::Ptr &doc,
                   const QStringList importPaths)
        : m_snapshot(snapshot)
        , m_doc(doc)
        , m_link(snapshot, importPaths,
                 QmlJS::ModelManagerInterface::instance()->builtins(doc))
        , m_context(m_link(doc, &m_diagnosticLinkMessages))
        , m_scopeChain(doc, m_context)
        , m_scopeBuilder(&m_scopeChain)
    {
    }

    ~ReadingContext()
    {}

    Document::Ptr doc() const
    { return m_doc; }

    void enterScope(Node *node)
    { m_scopeBuilder.push(node); }

    void leaveScope()
    { m_scopeBuilder.pop(); }

    void lookup(UiQualifiedId *astTypeNode, QString &typeName, int &majorVersion,
                int &minorVersion, QString &defaultPropertyName)
    {
        const ObjectValue *value = m_context->lookupType(m_doc.data(), astTypeNode);
        defaultPropertyName = m_context->defaultPropertyName(value);

        const CppComponentValue * qmlValue = value_cast<CppComponentValue>(value);
        if (qmlValue) {
            typeName = qmlValue->moduleName() + QLatin1String(".") + qmlValue->className();

            majorVersion = qmlValue->componentVersion().majorVersion();
            minorVersion = qmlValue->componentVersion().minorVersion();
        } else {
            for (UiQualifiedId *iter = astTypeNode; iter; iter = iter->next)
                if (!iter->next && !iter->name.isEmpty())
                    typeName = iter->name.toString();

            QString fullTypeName;
            for (UiQualifiedId *iter = astTypeNode; iter; iter = iter->next)
                if (!iter->name.isEmpty())
                    fullTypeName += iter->name.toString() + QLatin1Char('.');

            if (fullTypeName.endsWith(QLatin1Char('.')))
                fullTypeName.chop(1);

            majorVersion = ComponentVersion::NoVersion;
            minorVersion = ComponentVersion::NoVersion;

            const Imports *imports = m_context->imports(m_doc.data());
            ImportInfo importInfo = imports->info(fullTypeName, m_context.data());
            if (importInfo.isValid() && importInfo.type() == ImportInfo::LibraryImport) {
                QString name = importInfo.name();
                majorVersion = importInfo.version().majorVersion();
                minorVersion = importInfo.version().minorVersion();
                typeName.prepend(name + QLatin1Char('.'));
            } else if (importInfo.isValid() && importInfo.type() == ImportInfo::DirectoryImport) {
                QString path = importInfo.path();
                QDir dir(m_doc->path());
                // should probably try to make it relatve to some import path, not to the document path
                QString relativeDir = dir.relativeFilePath(path);
                QString name = relativeDir.replace(QLatin1Char('/'), QLatin1Char('.'));
                if (!name.isEmpty())
                    typeName.prepend(name + QLatin1Char('.'));
            } else if (importInfo.isValid() && importInfo.type() == ImportInfo::QrcDirectoryImport) {
                QString path = QrcParser::normalizedQrcDirectoryPath(importInfo.path());
                path = path.mid(1, path.size() - ((path.size() > 1) ? 2 : 1));
                const QString name = path.replace(QLatin1Char('/'), QLatin1Char('.'));
                if (!name.isEmpty())
                    typeName.prepend(name + QLatin1Char('.'));
            }
        }
    }

    /// When something is changed here, also change Check::checkScopeObjectMember in
    /// qmljscheck.cpp
    /// ### Maybe put this into the context as a helper method.
    bool lookupProperty(const QString &prefix, const UiQualifiedId *id, const Value **property = 0, const ObjectValue **parentObject = 0, QString *name = 0)
    {
        QList<const ObjectValue *> scopeObjects = m_scopeChain.qmlScopeObjects();
        if (scopeObjects.isEmpty())
            return false;

        if (! id)
            return false; // ### error?

        if (id->name.isEmpty()) // possible after error recovery
            return false;

        QString propertyName;
        if (prefix.isEmpty())
            propertyName = id->name.toString();
        else
            propertyName = prefix;

        if (name)
            *name = propertyName;

        if (propertyName == QLatin1String("id") && ! id->next)
            return false; // ### should probably be a special value

        // attached properties
        bool isAttachedProperty = false;
        if (! propertyName.isEmpty() && propertyName[0].isUpper()) {
            isAttachedProperty = true;
            if (const ObjectValue *qmlTypes = m_scopeChain.qmlTypes())
                scopeObjects += qmlTypes;
        }

        if (scopeObjects.isEmpty())
            return false;

        // global lookup for first part of id
        const ObjectValue *objectValue = 0;
        const Value *value = 0;
        for (int i = scopeObjects.size() - 1; i >= 0; --i) {
            objectValue = scopeObjects[i];
            value = objectValue->lookupMember(propertyName, m_context);
            if (value)
                break;
        }
        if (parentObject)
            *parentObject = objectValue;
        if (!value) {
            qWarning() << "Skipping invalid property name" << propertyName;
            return false;
        }

        // can't look up members for attached properties
        if (isAttachedProperty)
            return false;

        // resolve references
        if (const Reference *ref = value->asReference())
            value = m_context->lookupReference(ref);

        // member lookup
        const UiQualifiedId *idPart = id;
        if (prefix.isEmpty())
            idPart = idPart->next;
        for (; idPart; idPart = idPart->next) {
            objectValue = value_cast<ObjectValue>(value);
            if (! objectValue) {
//                if (idPart->name)
//                    qDebug() << idPart->name->asString() << "has no property named"
//                             << propertyName;
                return false;
            }
            if (parentObject)
                *parentObject = objectValue;

            if (idPart->name.isEmpty()) {
                // somebody typed "id." and error recovery still gave us a valid tree,
                // so just bail out here.
                return false;
            }

            propertyName = idPart->name.toString();
            if (name)
                *name = propertyName;

            value = objectValue->lookupMember(propertyName, m_context);
            if (! value) {
//                if (idPart->name)
//                    qDebug() << "In" << idPart->name->asString() << ":"
//                             << objectValue->className() << "has no property named"
//                             << propertyName;
                return false;
            }
        }

        if (property)
            *property = value;
        return true;
    }

    bool isArrayProperty(const Value *value, const ObjectValue *containingObject, const QString &name)
    {
        if (!value)
            return false;
        const ObjectValue *objectValue = value->asObjectValue();
        if (objectValue && objectValue->prototype(m_context) == m_context->valueOwner()->arrayPrototype())
            return true;

        PrototypeIterator iter(containingObject, m_context);
        while (iter.hasNext()) {
            const ObjectValue *proto = iter.next();
            if (proto->lookupMember(name, m_context) == m_context->valueOwner()->arrayPrototype())
                return true;
            if (const CppComponentValue *qmlIter = value_cast<CppComponentValue>(proto)) {
                if (qmlIter->isListProperty(name))
                    return true;
            }
        }
        return false;
    }

    QVariant convertToVariant(const QString &astValue, const QString &propertyPrefix, UiQualifiedId *propertyId)
    {
        const bool hasQuotes = astValue.trimmed().left(1) == QLatin1String("\"") && astValue.trimmed().right(1) == QLatin1String("\"");
        const QString cleanedValue = fixEscapedUnicodeChar(deEscape(stripQuotes(astValue.trimmed())));
        const Value *property = 0;
        const ObjectValue *containingObject = 0;
        QString name;
        if (!lookupProperty(propertyPrefix, propertyId, &property, &containingObject, &name)) {
            qWarning() << "Unknown property" << propertyPrefix + QLatin1Char('.') + toString(propertyId)
                       << "on line" << propertyId->identifierToken.startLine
                       << "column" << propertyId->identifierToken.startColumn;
            return hasQuotes ? QVariant(cleanedValue) : cleverConvert(cleanedValue);
        }

        if (containingObject)
            containingObject->lookupMember(name, m_context, &containingObject);

        if (const CppComponentValue * qmlObject = value_cast<CppComponentValue>(containingObject)) {
            const QString typeName = qmlObject->propertyType(name);
            if (qmlObject->getEnum(typeName).isValid()) {
                return QVariant(cleanedValue);
            } else {
                int type = QMetaType::type(typeName.toUtf8().constData());
                QVariant result;
                if (type)
                    result = PropertyParser::read(type, cleanedValue);
                if (result.isValid())
                    return result;
            }
        }

        if (property->asColorValue())
            return PropertyParser::read(QVariant::Color, cleanedValue);
        else if (property->asUrlValue())
            return PropertyParser::read(QVariant::Url, cleanedValue);

        QVariant value(cleanedValue);
        if (property->asBooleanValue()) {
            value.convert(QVariant::Bool);
            return value;
        } else if (property->asNumberValue()) {
            value.convert(QVariant::Double);
            return value;
        } else if (property->asStringValue()) {
            // nothing to do
        } else { //property alias et al
            if (!hasQuotes)
                return cleverConvert(cleanedValue);
        }
        return value;
    }

    QVariant convertToEnum(Statement *rhs, const QString &propertyPrefix, UiQualifiedId *propertyId, const QString &astValue)
    {
        QStringList astValueList = astValue.split(QLatin1String("."));

        if (astValueList.count() == 2 //Check for global Qt enums
                && astValueList.first() == QLatin1String("Qt")
                && globalQtEnums().contains(astValueList.last()))
            return QVariant(astValueList.last());

        ExpressionStatement *eStmt = cast<ExpressionStatement *>(rhs);
        if (!eStmt || !eStmt->expression)
            return QVariant();

        const ObjectValue *containingObject = 0;
        QString name;
        if (!lookupProperty(propertyPrefix, propertyId, 0, &containingObject, &name))
            return QVariant();

        if (containingObject)
            containingObject->lookupMember(name, m_context, &containingObject);
        const CppComponentValue * lhsCppComponent = value_cast<CppComponentValue>(containingObject);
        if (!lhsCppComponent)
            return QVariant();
        const QString lhsPropertyTypeName = lhsCppComponent->propertyType(name);

        const ObjectValue *rhsValueObject = 0;
        QString rhsValueName;
        if (IdentifierExpression *idExp = cast<IdentifierExpression *>(eStmt->expression)) {
            if (!m_scopeChain.qmlScopeObjects().isEmpty())
                rhsValueObject = m_scopeChain.qmlScopeObjects().last();
            if (!idExp->name.isEmpty())
                rhsValueName = idExp->name.toString();
        } else if (FieldMemberExpression *memberExp = cast<FieldMemberExpression *>(eStmt->expression)) {
            Evaluate evaluate(&m_scopeChain);
            const Value *result = evaluate(memberExp->base);
            rhsValueObject = result->asObjectValue();

            if (!memberExp->name.isEmpty())
                rhsValueName = memberExp->name.toString();
        }

        if (rhsValueObject)
            rhsValueObject->lookupMember(rhsValueName, m_context, &rhsValueObject);

        const CppComponentValue *rhsCppComponentValue = value_cast<CppComponentValue>(rhsValueObject);
        if (!rhsCppComponentValue)
            return QVariant();

        if (rhsCppComponentValue->getEnum(lhsPropertyTypeName).hasKey(rhsValueName))
            return QVariant(rhsValueName);
        else
            return QVariant();
    }


    const ScopeChain &scopeChain() const
    { return m_scopeChain; }

    QList<DiagnosticMessage> diagnosticLinkMessages() const
    { return m_diagnosticLinkMessages; }

private:
    Snapshot m_snapshot;
    Document::Ptr m_doc;
    Link m_link;
    QList<DiagnosticMessage> m_diagnosticLinkMessages;
    ContextPtr m_context;
    ScopeChain m_scopeChain;
    ScopeBuilder m_scopeBuilder;
};

} // namespace Internal
} // namespace QmlDesigner

using namespace QmlDesigner;
using namespace QmlDesigner::Internal;


static inline bool smartVeryFuzzyCompare(QVariant value1, QVariant value2)
{ //we ignore slight changes on doubles and only check three digits
    if ((value1.type() == QVariant::Double) || (value2.type() == QVariant::Double)) {
        bool ok1, ok2;
        qreal a = value1.toDouble(&ok1);
        qreal b = value2.toDouble(&ok2);

        if (!ok1 || !ok2)
            return false;

        if (qFuzzyCompare(a, b))
            return true;

        int ai = qRound(a * 1000);
        int bi = qRound(b * 1000);

        if (qFuzzyCompare((qreal(ai) / 1000), (qreal(bi) / 1000)))
            return true;
    }
    return false;
}

static inline bool equals(const QVariant &a, const QVariant &b)
{
    if (a == b)
        return true;
    if (smartVeryFuzzyCompare(a, b))
        return true;
    return false;
}

TextToModelMerger::TextToModelMerger(RewriterView *reWriterView) :
        m_rewriterView(reWriterView),
        m_isActive(false)
{
    Q_ASSERT(reWriterView);
    m_setupTimer.setSingleShot(true);
    RewriterView::connect(&m_setupTimer, SIGNAL(timeout()), reWriterView, SLOT(delayedSetup()));
}

void TextToModelMerger::setActive(bool active)
{
    m_isActive = active;
}

bool TextToModelMerger::isActive() const
{
    return m_isActive;
}

void TextToModelMerger::setupImports(const Document::Ptr &doc,
                                     DifferenceHandler &differenceHandler)
{
    QList<Import> existingImports = m_rewriterView->model()->imports();

    for (UiImportList *iter = doc->qmlProgram()->imports; iter; iter = iter->next) {
        UiImport *import = iter->import;
        if (!import)
            continue;

        QString version;
        if (import->versionToken.isValid())
            version = textAt(doc, import->versionToken);
        const QString &as = import->importId.toString();

        if (!import->fileName.isEmpty()) {
            const QString strippedFileName = stripQuotes(import->fileName.toString());
            const Import newImport = Import::createFileImport(strippedFileName,
                                                              version, as, m_rewriterView->textModifier()->importPaths());

            if (!existingImports.removeOne(newImport))
                differenceHandler.modelMissesImport(newImport);
        } else {
            QString importUri = toString(import->importUri);
            if (importUri == QLatin1String("Qt") && version == QLatin1String("4.7")) {
                importUri = QLatin1String("QtQuick");
                version = QLatin1String("1.0");
            }

            const Import newImport =
                    Import::createLibraryImport(importUri, version, as, m_rewriterView->textModifier()->importPaths());

            if (!existingImports.removeOne(newImport))
                differenceHandler.modelMissesImport(newImport);
        }
    }

    foreach (const Import &import, existingImports)
        differenceHandler.importAbsentInQMl(import);
}

bool TextToModelMerger::load(const QString &data, DifferenceHandler &differenceHandler)
{
//    qDebug() << "TextToModelMerger::load with data:" << data;

    const QUrl url = m_rewriterView->model()->fileUrl();
    const QStringList importPaths = m_rewriterView->textModifier()->importPaths();
    setActive(true);


    try {
        Snapshot snapshot = m_rewriterView->textModifier()->getSnapshot();
        const QString fileName = url.toLocalFile();
        Document::MutablePtr doc = Document::create(fileName.isEmpty() ? QLatin1String("<internal>") : fileName, Document::QmlLanguage);
        doc->setSource(data);
        doc->parseQml();

        if (!doc->isParsedCorrectly()) {
            QList<RewriterView::Error> errors;
            foreach (const QmlJS::DiagnosticMessage &message, doc->diagnosticMessages())
                errors.append(RewriterView::Error(message, QUrl::fromLocalFile(doc->fileName())));
            m_rewriterView->setErrors(errors);
            setActive(false);
            return false;
        }
        snapshot.insert(doc);
        ReadingContext ctxt(snapshot, doc, importPaths);
        m_scopeChain = QSharedPointer<const ScopeChain>(
                    new ScopeChain(ctxt.scopeChain()));
        m_document = doc;

        QList<RewriterView::Error> errors;
        QList<RewriterView::Error> warnings;

        foreach (const QmlJS::DiagnosticMessage &diagnosticMessage, ctxt.diagnosticLinkMessages()) {
            errors.append(RewriterView::Error(diagnosticMessage, QUrl::fromLocalFile(doc->fileName())));
        }

        setupImports(doc, differenceHandler);

        if (m_rewriterView->model()->imports().isEmpty()) {
            const QmlJS::DiagnosticMessage diagnosticMessage(QmlJS::DiagnosticMessage::Error, AST::SourceLocation(0, 0, 0, 0), QCoreApplication::translate("QmlDesigner::TextToModelMerger", "No import statements found"));
            errors.append(RewriterView::Error(diagnosticMessage, QUrl::fromLocalFile(doc->fileName())));
        }

        foreach (const QmlDesigner::Import &import, m_rewriterView->model()->imports()) {
            if (import.isLibraryImport() && import.url() == QLatin1String("QtQuick") && !supportedQtQuickVersion(import.version())) {
                const QmlJS::DiagnosticMessage diagnosticMessage(QmlJS::DiagnosticMessage::Error, AST::SourceLocation(0, 0, 0, 0),
                                                                 QCoreApplication::translate("QmlDesigner::TextToModelMerger", "Unsupported QtQuick version"));
                errors.append(RewriterView::Error(diagnosticMessage, QUrl::fromLocalFile(doc->fileName())));
            }
        }

        if (view()->checkSemanticErrors()) {
            Check check(doc, m_scopeChain->context());
            check.disableMessage(StaticAnalysis::ErrUnknownComponent);
            check.disableMessage(StaticAnalysis::ErrPrototypeCycle);
            check.disableMessage(StaticAnalysis::ErrCouldNotResolvePrototype);
            check.disableMessage(StaticAnalysis::ErrCouldNotResolvePrototypeOf);

            foreach (StaticAnalysis::Type type, StaticAnalysis::Message::allMessageTypes()) {
                StaticAnalysis::PrototypeMessageData prototypeMessageData = StaticAnalysis::Message::prototypeForMessageType(type);
                if (prototypeMessageData.severity == StaticAnalysis::MaybeWarning
                        || prototypeMessageData.severity == StaticAnalysis::Warning) {
                    check.disableMessage(type);
                }
            }

            check.enableMessage(StaticAnalysis::WarnImperativeCodeNotEditableInVisualDesigner);
            check.enableMessage(StaticAnalysis::WarnUnsupportedTypeInVisualDesigner);
            check.enableMessage(StaticAnalysis::WarnReferenceToParentItemNotSupportedByVisualDesigner);
            //## triggers too often ## check.enableMessage(StaticAnalysis::WarnUndefinedValueForVisualDesigner);
            check.enableMessage(StaticAnalysis::WarnStatesOnlyInRootItemForVisualDesigner);

            foreach (const StaticAnalysis::Message &message, check()) {
                if (message.severity == StaticAnalysis::Error)
                    errors.append(RewriterView::Error(message.toDiagnosticMessage(), QUrl::fromLocalFile(doc->fileName())));
                if (message.severity == StaticAnalysis::Warning)
                    warnings.append(RewriterView::Error(message.toDiagnosticMessage(), QUrl::fromLocalFile(doc->fileName())));
            }

            if (!errors.isEmpty()) {
                m_rewriterView->setErrors(errors);
                setActive(false);
                return false;
            }

            if (!warnings.isEmpty() && differenceHandler.isValidator() && !m_rewriterView->inErrorState()) {

                QString title = QCoreApplication::translate("QmlDesigner::TextToModelMerger", "This .qml file contains features "
                                                            "which are not supported by Qt Quick Designer");

                QStringList message;

                foreach (const RewriterView::Error &warning, warnings) {
                    QString string = QLatin1String("Line: ") +  QString::number(warning.line()) + QLatin1String(": ")  + warning.description();
                    //string += QLatin1String(" <a href=\"") + QString::number(warning.line()) + QLatin1String("\">Go to error</a>") + QLatin1String("<p>");
                    message << string;
                }

                QmlWarningDialog warningDialog(0, message);
                if (warningDialog.warningsEnabled() && warningDialog.exec()) {
                    m_rewriterView->setErrors(warnings);
                    setActive(false);
                    return false;
                }
            }
        }

        UiObjectMember *astRootNode = 0;
        if (UiProgram *program = doc->qmlProgram())
            if (program->members)
                astRootNode = program->members->member;
        ModelNode modelRootNode = m_rewriterView->rootModelNode();
        syncNode(modelRootNode, astRootNode, &ctxt, differenceHandler);
        m_rewriterView->positionStorage()->cleanupInvalidOffsets();
        m_rewriterView->clearErrors();

        setActive(false);
        return true;
    } catch (Exception &e) {
        RewriterView::Error error(&e);
        // Somehow, the error below gets eaten in upper levels, so printing the
        // exception info here for debugging purposes:
        qDebug() << "*** An exception occurred while reading the QML file:"
                 << error.toString();
        m_rewriterView->addError(error);

        setActive(false);

        return false;
    }
}

void TextToModelMerger::syncNode(ModelNode &modelNode,
                                 UiObjectMember *astNode,
                                 ReadingContext *context,
                                 DifferenceHandler &differenceHandler)
{
    UiQualifiedId *astObjectType = qualifiedTypeNameId(astNode);
    UiObjectInitializer *astInitializer = initializerOfObject(astNode);

    if (!astObjectType || !astInitializer)
        return;

    m_rewriterView->positionStorage()->setNodeOffset(modelNode, astObjectType->identifierToken.offset);

    QString typeNameString;
    QString defaultPropertyNameString;
    int majorVersion;
    int minorVersion;
    context->lookup(astObjectType, typeNameString, majorVersion, minorVersion, defaultPropertyNameString);

    TypeName typeName = typeNameString.toUtf8();
    PropertyName defaultPropertyName = defaultPropertyNameString.toUtf8();

    if (defaultPropertyName.isEmpty()) //fallback and use the meta system of the model
        defaultPropertyName = modelNode.metaInfo().defaultPropertyName();

    if (typeName.isEmpty()) {
        qWarning() << "Skipping node with unknown type" << toString(astObjectType);
        return;
    }

    if (modelNode.isRootNode() && isComponentType(typeName)) {
        for (UiObjectMemberList *iter = astInitializer->members; iter; iter = iter->next) {
            if (UiObjectDefinition *def = cast<UiObjectDefinition *>(iter->member)) {
                syncNode(modelNode, def, context, differenceHandler);
                return;
            }
        }
    }

    bool isImplicitComponent = modelNode.parentProperty().isValid() && propertyIsComponentType(modelNode.parentProperty(), typeName, modelNode.model());


    if (modelNode.type() != typeName //If there is no valid parentProperty                                                                                                      //the node has just been created. The type is correct then.
            || modelNode.majorVersion() != majorVersion
            || modelNode.minorVersion() != minorVersion) {
        const bool isRootNode = m_rewriterView->rootModelNode() == modelNode;
        differenceHandler.typeDiffers(isRootNode, modelNode, typeName,
                                      majorVersion, minorVersion,
                                      astNode, context);
        if (!isRootNode)
            return; // the difference handler will create a new node, so we're done.
    }

    if (isComponentType(typeName) || isImplicitComponent)
        setupComponentDelayed(modelNode, differenceHandler.isValidator());

    if (isCustomParserType(typeName))
        setupCustomParserNodeDelayed(modelNode, differenceHandler.isValidator());

    context->enterScope(astNode);

    QSet<PropertyName> modelPropertyNames = QSet<PropertyName>::fromList(modelNode.propertyNames());
    if (!modelNode.id().isEmpty())
        modelPropertyNames.insert("id");
    QList<UiObjectMember *> defaultPropertyItems;

    for (UiObjectMemberList *iter = astInitializer->members; iter; iter = iter->next) {
        UiObjectMember *member = iter->member;
        if (!member)
            continue;

        if (UiArrayBinding *array = cast<UiArrayBinding *>(member)) {
            const QString astPropertyName = toString(array->qualifiedId);
            if (isPropertyChangesType(typeName) || isConnectionsType(typeName) || context->lookupProperty(QString(), array->qualifiedId)) {
                AbstractProperty modelProperty = modelNode.property(astPropertyName.toUtf8());
                QList<UiObjectMember *> arrayMembers;
                for (UiArrayMemberList *iter = array->members; iter; iter = iter->next)
                    if (UiObjectMember *member = iter->member)
                        arrayMembers.append(member);

                syncArrayProperty(modelProperty, arrayMembers, context, differenceHandler);
                modelPropertyNames.remove(astPropertyName.toUtf8());
            } else {
                qWarning() << "Skipping invalid array property" << astPropertyName
                           << "for node type" << modelNode.type();
            }
        } else if (UiObjectDefinition *def = cast<UiObjectDefinition *>(member)) {
            const QString &name = def->qualifiedTypeNameId->name.toString();
            if (name.isEmpty() || !name.at(0).isUpper()) {
                QStringList props = syncGroupedProperties(modelNode,
                                                          name,
                                                          def->initializer->members,
                                                          context,
                                                          differenceHandler);
                foreach (const QString &prop, props)
                    modelPropertyNames.remove(prop.toUtf8());
            } else {
                defaultPropertyItems.append(member);
            }
        } else if (UiObjectBinding *binding = cast<UiObjectBinding *>(member)) {
            const QString astPropertyName = toString(binding->qualifiedId);
            if (binding->hasOnToken) {
                // skip value sources
            } else {
                const Value *propertyType = 0;
                const ObjectValue *containingObject = 0;
                QString name;
                if (context->lookupProperty(QString(), binding->qualifiedId, &propertyType, &containingObject, &name)
                        || isPropertyChangesType(typeName)
                        || isConnectionsType(typeName)) {
                    AbstractProperty modelProperty = modelNode.property(astPropertyName.toUtf8());
                    if (context->isArrayProperty(propertyType, containingObject, name))
                        syncArrayProperty(modelProperty, QList<QmlJS::AST::UiObjectMember*>() << member, context, differenceHandler);
                    else
                        syncNodeProperty(modelProperty, binding, context, differenceHandler);
                    modelPropertyNames.remove(astPropertyName.toUtf8());
                } else {
                    qWarning() << "Skipping invalid node property" << astPropertyName
                               << "for node type" << modelNode.type();
                }
            }
        } else if (UiScriptBinding *script = cast<UiScriptBinding *>(member)) {
            modelPropertyNames.remove(syncScriptBinding(modelNode, QString(), script, context, differenceHandler));
        } else if (UiPublicMember *property = cast<UiPublicMember *>(member)) {
            if (property->type == UiPublicMember::Signal)
                continue; // QML designer doesn't support this yet.

            if (property->name.isEmpty() || property->memberType.isEmpty())
                continue; // better safe than sorry.

            const QString &astName = property->name.toString();
            QString astValue;
            if (property->statement)
                astValue = textAt(context->doc(),
                                  property->statement->firstSourceLocation(),
                                  property->statement->lastSourceLocation());
            const TypeName &astType = property->memberType.toUtf8();
            AbstractProperty modelProperty = modelNode.property(astName.toUtf8());
            if (!property->statement || isLiteralValue(property->statement)) {
                const QVariant variantValue = convertDynamicPropertyValueToVariant(astValue, astType);
                syncVariantProperty(modelProperty, variantValue, astType, differenceHandler);
            } else {
                syncExpressionProperty(modelProperty, astValue, astType, differenceHandler);
            }
            modelPropertyNames.remove(astName.toUtf8());
        } else {
            qWarning() << "Found an unknown QML value.";
        }
    }

    if (!defaultPropertyItems.isEmpty()) {
        if (isComponentType(modelNode.type()))
            setupComponentDelayed(modelNode, differenceHandler.isValidator());
        if (defaultPropertyName.isEmpty()) {
            qWarning() << "No default property for node type" << modelNode.type() << ", ignoring child items.";
        } else {
            AbstractProperty modelProperty = modelNode.property(defaultPropertyName);
            if (modelProperty.isNodeListProperty()) {
                NodeListProperty nodeListProperty = modelProperty.toNodeListProperty();
                syncNodeListProperty(nodeListProperty, defaultPropertyItems, context,
                                     differenceHandler);
            } else {
                differenceHandler.shouldBeNodeListProperty(modelProperty,
                                                           defaultPropertyItems,
                                                           context);
            }
            modelPropertyNames.remove(defaultPropertyName);
        }
    }

    foreach (const PropertyName &modelPropertyName, modelPropertyNames) {
        AbstractProperty modelProperty = modelNode.property(modelPropertyName);

        // property deleted.
        if (modelPropertyName == "id")
            differenceHandler.idsDiffer(modelNode, QString());
        else
            differenceHandler.propertyAbsentFromQml(modelProperty);
    }

    context->leaveScope();
}

QmlDesigner::PropertyName TextToModelMerger::syncScriptBinding(ModelNode &modelNode,
                                             const QString &prefix,
                                             UiScriptBinding *script,
                                             ReadingContext *context,
                                             DifferenceHandler &differenceHandler)
{
    QString astPropertyName = toString(script->qualifiedId);
    if (!prefix.isEmpty())
        astPropertyName.prepend(prefix + QLatin1Char('.'));

    QString astValue;
    if (script->statement) {
        astValue = textAt(context->doc(),
                          script->statement->firstSourceLocation(),
                          script->statement->lastSourceLocation());
        astValue = astValue.trimmed();
        if (astValue.endsWith(QLatin1Char(';')))
            astValue = astValue.left(astValue.length() - 1);
        astValue = astValue.trimmed();
    }

    if (astPropertyName == QLatin1String("id")) {
        syncNodeId(modelNode, astValue, differenceHandler);
        return astPropertyName.toUtf8();
    }

    if (isSignalPropertyName(astPropertyName)) {
        AbstractProperty modelProperty = modelNode.property(astPropertyName.toUtf8());
        syncSignalHandler(modelProperty, astValue, differenceHandler);
        return astPropertyName.toUtf8();
    }

    if (isLiteralValue(script)) {
        if (isPropertyChangesType(modelNode.type())
                || isConnectionsType(modelNode.type())) {
            AbstractProperty modelProperty = modelNode.property(astPropertyName.toUtf8());
            const QVariant variantValue(deEscape(stripQuotes(astValue)));
            syncVariantProperty(modelProperty, variantValue, TypeName(), differenceHandler);
            return astPropertyName.toUtf8();
        } else {
            const QVariant variantValue = context->convertToVariant(astValue, prefix, script->qualifiedId);
            if (variantValue.isValid()) {
                AbstractProperty modelProperty = modelNode.property(astPropertyName.toUtf8());
                syncVariantProperty(modelProperty, variantValue, TypeName(), differenceHandler);
                return astPropertyName.toUtf8();
            } else {
                qWarning() << "Skipping invalid variant property" << astPropertyName
                           << "for node type" << modelNode.type();
                return PropertyName();
            }
        }
    }

    const QVariant enumValue = context->convertToEnum(script->statement, prefix, script->qualifiedId, astValue);
    if (enumValue.isValid()) { // It is a qualified enum:
        AbstractProperty modelProperty = modelNode.property(astPropertyName.toUtf8());
        syncVariantProperty(modelProperty, enumValue, TypeName(), differenceHandler); // TODO: parse type
        return astPropertyName.toUtf8();
    } else { // Not an enum, so:
        if (isPropertyChangesType(modelNode.type())
                || isConnectionsType(modelNode.type())
                || context->lookupProperty(prefix, script->qualifiedId)) {
            AbstractProperty modelProperty = modelNode.property(astPropertyName.toUtf8());
            syncExpressionProperty(modelProperty, astValue, TypeName(), differenceHandler); // TODO: parse type
            return astPropertyName.toUtf8();
        } else {
            qWarning() << "Skipping invalid expression property" << astPropertyName
                    << "for node type" << modelNode.type();
            return PropertyName();
        }
    }
}

void TextToModelMerger::syncNodeId(ModelNode &modelNode, const QString &astObjectId,
                                   DifferenceHandler &differenceHandler)
{
    if (astObjectId.isEmpty()) {
        if (!modelNode.id().isEmpty()) {
            ModelNode existingNodeWithId = m_rewriterView->modelNodeForId(astObjectId);
            if (existingNodeWithId.isValid())
                existingNodeWithId.setId(QString());
            differenceHandler.idsDiffer(modelNode, astObjectId);
        }
    } else {
        if (modelNode.id() != astObjectId) {
            ModelNode existingNodeWithId = m_rewriterView->modelNodeForId(astObjectId);
            if (existingNodeWithId.isValid())
                existingNodeWithId.setId(QString());
            differenceHandler.idsDiffer(modelNode, astObjectId);
        }
    }
}

void TextToModelMerger::syncNodeProperty(AbstractProperty &modelProperty,
                                         UiObjectBinding *binding,
                                         ReadingContext *context,
                                         DifferenceHandler &differenceHandler)
{
    QString typeNameString;
    QString dummy;
    int majorVersion;
    int minorVersion;
    context->lookup(binding->qualifiedTypeNameId, typeNameString, majorVersion, minorVersion, dummy);

    TypeName typeName = typeNameString.toUtf8();

    if (typeName.isEmpty()) {
        qWarning() << "Skipping node with unknown type" << toString(binding->qualifiedTypeNameId);
        return;
    }

    if (modelProperty.isNodeProperty()) {
        ModelNode nodePropertyNode = modelProperty.toNodeProperty().modelNode();
        syncNode(nodePropertyNode, binding, context, differenceHandler);
    } else {
        differenceHandler.shouldBeNodeProperty(modelProperty,
                                               typeName,
                                               majorVersion,
                                               minorVersion,
                                               binding,
                                               context);
    }
}

void TextToModelMerger::syncExpressionProperty(AbstractProperty &modelProperty,
                                               const QString &javascript,
                                               const TypeName &astType,
                                               DifferenceHandler &differenceHandler)
{
    if (modelProperty.isBindingProperty()) {
        BindingProperty bindingProperty = modelProperty.toBindingProperty();
        if (bindingProperty.expression() != javascript
                || !astType.isEmpty() != bindingProperty.isDynamic()
                || astType != bindingProperty.dynamicTypeName()) {
            differenceHandler.bindingExpressionsDiffer(bindingProperty, javascript, astType);
        }
    } else {
        differenceHandler.shouldBeBindingProperty(modelProperty, javascript, astType);
    }
}

void TextToModelMerger::syncSignalHandler(AbstractProperty &modelProperty,
                                               const QString &javascript,
                                               DifferenceHandler &differenceHandler)
{
    if (modelProperty.isSignalHandlerProperty()) {
        SignalHandlerProperty signalHandlerProperty = modelProperty.toSignalHandlerProperty();
        if (signalHandlerProperty.source() != javascript) {
            differenceHandler.signalHandlerSourceDiffer(signalHandlerProperty, javascript);
        }
    } else {
        differenceHandler.shouldBeSignalHandlerProperty(modelProperty, javascript);
    }
}


void TextToModelMerger::syncArrayProperty(AbstractProperty &modelProperty,
                                          const QList<UiObjectMember *> &arrayMembers,
                                          ReadingContext *context,
                                          DifferenceHandler &differenceHandler)
{
    if (modelProperty.isNodeListProperty()) {
        NodeListProperty nodeListProperty = modelProperty.toNodeListProperty();
        syncNodeListProperty(nodeListProperty, arrayMembers, context, differenceHandler);
    } else {
        differenceHandler.shouldBeNodeListProperty(modelProperty,
                                                   arrayMembers,
                                                   context);
    }
}

void TextToModelMerger::syncVariantProperty(AbstractProperty &modelProperty,
                                            const QVariant &astValue,
                                            const TypeName &astType,
                                            DifferenceHandler &differenceHandler)
{
    if (modelProperty.isVariantProperty()) {
        VariantProperty modelVariantProperty = modelProperty.toVariantProperty();

        if (!equals(modelVariantProperty.value(), astValue)
                || !astType.isEmpty() != modelVariantProperty.isDynamic()
                || astType != modelVariantProperty.dynamicTypeName()) {
            differenceHandler.variantValuesDiffer(modelVariantProperty,
                                                  astValue,
                                                  astType);
        }
    } else {
        differenceHandler.shouldBeVariantProperty(modelProperty,
                                                  astValue,
                                                  astType);
    }
}

void TextToModelMerger::syncNodeListProperty(NodeListProperty &modelListProperty,
                                             const QList<UiObjectMember *> arrayMembers,
                                             ReadingContext *context,
                                             DifferenceHandler &differenceHandler)
{
    QList<ModelNode> modelNodes = modelListProperty.toModelNodeList();
    int i = 0;
    for (; i < modelNodes.size() && i < arrayMembers.size(); ++i) {
        ModelNode modelNode = modelNodes.at(i);
        syncNode(modelNode, arrayMembers.at(i), context, differenceHandler);
    }

    for (int j = i; j < arrayMembers.size(); ++j) {
        // more elements in the dom-list, so add them to the model
        UiObjectMember *arrayMember = arrayMembers.at(j);
        const ModelNode newNode = differenceHandler.listPropertyMissingModelNode(modelListProperty, context, arrayMember);
    }

    for (int j = i; j < modelNodes.size(); ++j) {
        // more elements in the model, so remove them.
        ModelNode modelNode = modelNodes.at(j);
        differenceHandler.modelNodeAbsentFromQml(modelNode);
    }
}

ModelNode TextToModelMerger::createModelNode(const TypeName &typeName,
                                             int majorVersion,
                                             int minorVersion,
                                             bool isImplicitComponent,
                                             UiObjectMember *astNode,
                                             ReadingContext *context,
                                             DifferenceHandler &differenceHandler)
{
    QString nodeSource;

    UiQualifiedId *astObjectType = qualifiedTypeNameId(astNode);

    if (isCustomParserType(typeName))
        nodeSource = textAt(context->doc(),
                                    astObjectType->identifierToken.offset,
                                    astNode->lastSourceLocation());


    if (isComponentType(typeName) || isImplicitComponent) {
        QString componentSource = extractComponentFromQml(textAt(context->doc(),
                                  astObjectType->identifierToken.offset,
                                  astNode->lastSourceLocation()));


        nodeSource = componentSource;
    }

    ModelNode::NodeSourceType nodeSourceType = ModelNode::NodeWithoutSource;

    if (isComponentType(typeName) || isImplicitComponent)
        nodeSourceType = ModelNode::NodeWithComponentSource;
    else if (isCustomParserType(typeName))
        nodeSourceType = ModelNode::NodeWithCustomParserSource;

    ModelNode newNode = m_rewriterView->createModelNode(typeName,
                                                        majorVersion,
                                                        minorVersion,
                                                        PropertyListType(),
                                                        PropertyListType(),
                                                        nodeSource,
                                                        nodeSourceType);

    syncNode(newNode, astNode, context, differenceHandler);
    return newNode;
}

QStringList TextToModelMerger::syncGroupedProperties(ModelNode &modelNode,
                                                     const QString &name,
                                                     UiObjectMemberList *members,
                                                     ReadingContext *context,
                                                     DifferenceHandler &differenceHandler)
{
    QStringList props;

    for (UiObjectMemberList *iter = members; iter; iter = iter->next) {
        UiObjectMember *member = iter->member;

        if (UiScriptBinding *script = cast<UiScriptBinding *>(member)) {
            const QString prop = syncScriptBinding(modelNode, name, script, context, differenceHandler);
            if (!prop.isEmpty())
                props.append(prop);
        }
    }

    return props;
}

void ModelValidator::modelMissesImport(const QmlDesigner::Import &import)
{
    Q_UNUSED(import)
    Q_ASSERT(m_merger->view()->model()->imports().contains(import));
}

void ModelValidator::importAbsentInQMl(const QmlDesigner::Import &import)
{
    Q_UNUSED(import)
    Q_ASSERT(! m_merger->view()->model()->imports().contains(import));
}

void ModelValidator::bindingExpressionsDiffer(BindingProperty &modelProperty,
                                              const QString &javascript,
                                              const TypeName &astType)
{
    Q_UNUSED(modelProperty)
    Q_UNUSED(javascript)
    Q_UNUSED(astType)
    Q_ASSERT(modelProperty.expression() == javascript);
    Q_ASSERT(modelProperty.dynamicTypeName() == astType);
    Q_ASSERT(0);
}

void ModelValidator::shouldBeBindingProperty(AbstractProperty &modelProperty,
                                             const QString &/*javascript*/,
                                             const TypeName &/*astType*/)
{
    Q_UNUSED(modelProperty)
    Q_ASSERT(modelProperty.isBindingProperty());
    Q_ASSERT(0);
}

void ModelValidator::signalHandlerSourceDiffer(SignalHandlerProperty &modelProperty, const QString &javascript)
{
    Q_UNUSED(modelProperty)
    Q_UNUSED(javascript)
    Q_ASSERT(modelProperty.source() == javascript);
    Q_ASSERT(0);
}

void ModelValidator::shouldBeSignalHandlerProperty(AbstractProperty &modelProperty, const QString & /*javascript*/)
{
    Q_UNUSED(modelProperty)
    Q_ASSERT(modelProperty.isSignalHandlerProperty());
    Q_ASSERT(0);
}

void ModelValidator::shouldBeNodeListProperty(AbstractProperty &modelProperty,
                                              const QList<UiObjectMember *> /*arrayMembers*/,
                                              ReadingContext * /*context*/)
{
    Q_UNUSED(modelProperty)
    Q_ASSERT(modelProperty.isNodeListProperty());
    Q_ASSERT(0);
}

void ModelValidator::variantValuesDiffer(VariantProperty &modelProperty, const QVariant &qmlVariantValue, const TypeName &dynamicTypeName)
{
    Q_UNUSED(modelProperty)
    Q_UNUSED(qmlVariantValue)
    Q_UNUSED(dynamicTypeName)

    Q_ASSERT(modelProperty.isDynamic() == !dynamicTypeName.isEmpty());
    if (modelProperty.isDynamic()) {
        Q_ASSERT(modelProperty.dynamicTypeName() == dynamicTypeName);
    }

    Q_ASSERT(equals(modelProperty.value(), qmlVariantValue));
    Q_ASSERT(0);
}

void ModelValidator::shouldBeVariantProperty(AbstractProperty &modelProperty, const QVariant &/*qmlVariantValue*/, const TypeName &/*dynamicTypeName*/)
{
    Q_UNUSED(modelProperty)

    Q_ASSERT(modelProperty.isVariantProperty());
    Q_ASSERT(0);
}

void ModelValidator::shouldBeNodeProperty(AbstractProperty &modelProperty,
                                          const TypeName &/*typeName*/,
                                          int /*majorVersion*/,
                                          int /*minorVersion*/,
                                          UiObjectMember * /*astNode*/,
                                          ReadingContext * /*context*/)
{
    Q_UNUSED(modelProperty)

    Q_ASSERT(modelProperty.isNodeProperty());
    Q_ASSERT(0);
}

void ModelValidator::modelNodeAbsentFromQml(ModelNode &modelNode)
{
    Q_UNUSED(modelNode)

    Q_ASSERT(!modelNode.isValid());
    Q_ASSERT(0);
}

ModelNode ModelValidator::listPropertyMissingModelNode(NodeListProperty &/*modelProperty*/,
                                                       ReadingContext * /*context*/,
                                                       UiObjectMember * /*arrayMember*/)
{
    Q_ASSERT(0);
    return ModelNode();
}

void ModelValidator::typeDiffers(bool /*isRootNode*/,
                                 ModelNode &modelNode,
                                 const TypeName &typeName,
                                 int majorVersion,
                                 int minorVersion,
                                 QmlJS::AST::UiObjectMember * /*astNode*/,
                                 ReadingContext * /*context*/)
{
    Q_UNUSED(modelNode)
    Q_UNUSED(typeName)
    Q_UNUSED(minorVersion)
    Q_UNUSED(majorVersion)

    Q_ASSERT(modelNode.type() == typeName);
    Q_ASSERT(modelNode.majorVersion() == majorVersion);
    Q_ASSERT(modelNode.minorVersion() == minorVersion);
    Q_ASSERT(0);
}

void ModelValidator::propertyAbsentFromQml(AbstractProperty &modelProperty)
{
    Q_UNUSED(modelProperty)

    Q_ASSERT(!modelProperty.isValid());
    Q_ASSERT(0);
}

void ModelValidator::idsDiffer(ModelNode &modelNode, const QString &qmlId)
{
    Q_UNUSED(modelNode)
    Q_UNUSED(qmlId)

    Q_ASSERT(modelNode.id() == qmlId);
    Q_ASSERT(0);
}

void ModelAmender::modelMissesImport(const QmlDesigner::Import &import)
{
    m_merger->view()->model()->changeImports(QList<QmlDesigner::Import>() << import, QList<QmlDesigner::Import>());
}

void ModelAmender::importAbsentInQMl(const QmlDesigner::Import &import)
{
    m_merger->view()->model()->changeImports(QList<Import>(), QList<Import>() << import);
}

void ModelAmender::bindingExpressionsDiffer(BindingProperty &modelProperty,
                                            const QString &javascript,
                                            const TypeName &astType)
{
    if (astType.isEmpty())
        modelProperty.setExpression(javascript);
    else
        modelProperty.setDynamicTypeNameAndExpression(astType, javascript);
}

void ModelAmender::shouldBeBindingProperty(AbstractProperty &modelProperty,
                                           const QString &javascript,
                                           const TypeName &astType)
{
    ModelNode theNode = modelProperty.parentModelNode();
    BindingProperty newModelProperty = theNode.bindingProperty(modelProperty.name());
    if (astType.isEmpty())
        newModelProperty.setExpression(javascript);
    else
        newModelProperty.setDynamicTypeNameAndExpression(astType, javascript);
}

void ModelAmender::signalHandlerSourceDiffer(SignalHandlerProperty &modelProperty, const QString &javascript)
{
    modelProperty.setSource(javascript);
}

void ModelAmender::shouldBeSignalHandlerProperty(AbstractProperty &modelProperty, const QString &javascript)
{
    ModelNode theNode = modelProperty.parentModelNode();
    SignalHandlerProperty newModelProperty = theNode.signalHandlerProperty(modelProperty.name());
    newModelProperty.setSource(javascript);
}

void ModelAmender::shouldBeNodeListProperty(AbstractProperty &modelProperty,
                                            const QList<UiObjectMember *> arrayMembers,
                                            ReadingContext *context)
{
    ModelNode theNode = modelProperty.parentModelNode();
    NodeListProperty newNodeListProperty = theNode.nodeListProperty(modelProperty.name());
    m_merger->syncNodeListProperty(newNodeListProperty,
                                   arrayMembers,
                                   context,
                                   *this);
}



void ModelAmender::variantValuesDiffer(VariantProperty &modelProperty, const QVariant &qmlVariantValue, const TypeName &dynamicType)
{
//    qDebug()<< "ModelAmender::variantValuesDiffer for property"<<modelProperty.name()
//            << "in node" << modelProperty.parentModelNode().id()
//            << ", old value:" << modelProperty.value()
//            << "new value:" << qmlVariantValue;

    if (dynamicType.isEmpty())
        modelProperty.setValue(qmlVariantValue);
    else
        modelProperty.setDynamicTypeNameAndValue(dynamicType, qmlVariantValue);
}

void ModelAmender::shouldBeVariantProperty(AbstractProperty &modelProperty, const QVariant &qmlVariantValue, const TypeName &dynamicTypeName)
{
    ModelNode theNode = modelProperty.parentModelNode();
    VariantProperty newModelProperty = theNode.variantProperty(modelProperty.name());

    if (dynamicTypeName.isEmpty())
        newModelProperty.setValue(qmlVariantValue);
    else
        newModelProperty.setDynamicTypeNameAndValue(dynamicTypeName, qmlVariantValue);
}

void ModelAmender::shouldBeNodeProperty(AbstractProperty &modelProperty,
                                        const TypeName &typeName,
                                        int majorVersion,
                                        int minorVersion,
                                        UiObjectMember *astNode,
                                        ReadingContext *context)
{
    ModelNode theNode = modelProperty.parentModelNode();
    NodeProperty newNodeProperty = theNode.nodeProperty(modelProperty.name());

    const bool propertyTakesComponent = propertyIsComponentType(newNodeProperty, typeName, theNode.model());

    const ModelNode &newNode = m_merger->createModelNode(typeName,
                                                          majorVersion,
                                                          minorVersion,
                                                          propertyTakesComponent,
                                                          astNode,
                                                          context,
                                                          *this);

    newNodeProperty.setModelNode(newNode);

    if (propertyTakesComponent)
        m_merger->setupComponentDelayed(newNode, true);

}

void ModelAmender::modelNodeAbsentFromQml(ModelNode &modelNode)
{
    modelNode.destroy();
}

ModelNode ModelAmender::listPropertyMissingModelNode(NodeListProperty &modelProperty,
                                                     ReadingContext *context,
                                                     UiObjectMember *arrayMember)
{
    UiQualifiedId *astObjectType = 0;
    UiObjectInitializer *astInitializer = 0;
    if (UiObjectDefinition *def = cast<UiObjectDefinition *>(arrayMember)) {
        astObjectType = def->qualifiedTypeNameId;
        astInitializer = def->initializer;
    } else if (UiObjectBinding *bin = cast<UiObjectBinding *>(arrayMember)) {
        astObjectType = bin->qualifiedTypeNameId;
        astInitializer = bin->initializer;
    }

    if (!astObjectType || !astInitializer)
        return ModelNode();

    QString typeNameString;
    QString fullTypeName;
    QString dummy;
    int majorVersion;
    int minorVersion;
    context->lookup(astObjectType, typeNameString, majorVersion, minorVersion, dummy);

    TypeName typeName = typeNameString.toUtf8();

    if (typeName.isEmpty()) {
        qWarning() << "Skipping node with unknown type" << toString(astObjectType);
        return ModelNode();
    }

    const bool propertyTakesComponent = propertyIsComponentType(modelProperty, typeName, m_merger->view()->model());


    const ModelNode &newNode = m_merger->createModelNode(typeName,
                                                         majorVersion,
                                                         minorVersion,
                                                         propertyTakesComponent,
                                                         arrayMember,
                                                         context,
                                                         *this);


    if (propertyTakesComponent)
        m_merger->setupComponentDelayed(newNode, true);

    if (modelProperty.isDefaultProperty() || isComponentType(modelProperty.parentModelNode().type())) { //In the default property case we do some magic
        if (modelProperty.isNodeListProperty()) {
            modelProperty.reparentHere(newNode);
        } else { //The default property could a NodeProperty implicitly (delegate:)
            modelProperty.parentModelNode().removeProperty(modelProperty.name());
            modelProperty.reparentHere(newNode);
        }
    } else {
        modelProperty.reparentHere(newNode);
    }
    return newNode;
}

void ModelAmender::typeDiffers(bool isRootNode,
                               ModelNode &modelNode,
                               const TypeName &typeName,
                               int majorVersion,
                               int minorVersion,
                               QmlJS::AST::UiObjectMember *astNode,
                               ReadingContext *context)
{
    const bool propertyTakesComponent = propertyIsComponentType(modelNode.parentProperty(), typeName, modelNode.model());

    if (isRootNode) {
        modelNode.view()->changeRootNodeType(typeName, majorVersion, minorVersion);
    } else {
        NodeAbstractProperty parentProperty = modelNode.parentProperty();
        int nodeIndex = -1;
        if (parentProperty.isNodeListProperty()) {
            nodeIndex = parentProperty.toNodeListProperty().toModelNodeList().indexOf(modelNode);
            Q_ASSERT(nodeIndex >= 0);
        }

        modelNode.destroy();

        const ModelNode &newNode = m_merger->createModelNode(typeName,
                                                             majorVersion,
                                                             minorVersion,
                                                             propertyTakesComponent,
                                                             astNode,
                                                             context,
                                                             *this);
        parentProperty.reparentHere(newNode);
        if (parentProperty.isNodeListProperty()) {
            int currentIndex = parentProperty.toNodeListProperty().toModelNodeList().indexOf(newNode);
            if (nodeIndex != currentIndex)
                parentProperty.toNodeListProperty().slide(currentIndex, nodeIndex);
        }
    }
}

void ModelAmender::propertyAbsentFromQml(AbstractProperty &modelProperty)
{
    modelProperty.parentModelNode().removeProperty(modelProperty.name());
}

void ModelAmender::idsDiffer(ModelNode &modelNode, const QString &qmlId)
{
    modelNode.setId(qmlId);
}

void TextToModelMerger::setupComponent(const ModelNode &node)
{
    if (!node.isValid())
        return;

    QString componentText = m_rewriterView->extractText(QList<ModelNode>() << node).value(node);

    if (componentText.isEmpty())
        return;

    QString result = extractComponentFromQml(componentText);

    if (result.isEmpty())
        return; //No object definition found

    if (node.nodeSource() != result)
        ModelNode(node).setNodeSource(result);
}

void TextToModelMerger::setupComponentDelayed(const ModelNode &node, bool synchron)
{
    if (synchron) {
        setupComponent(node);
    } else {
        m_setupComponentList.insert(node);
        m_setupTimer.start();
    }
}

void TextToModelMerger::setupCustomParserNode(const ModelNode &node)
{
    if (!node.isValid())
        return;

    QString modelText = m_rewriterView->extractText(QList<ModelNode>() << node).value(node);

    if (modelText.isEmpty())
        return;

    if (node.nodeSource() != modelText)
        ModelNode(node).setNodeSource(modelText);

}

void TextToModelMerger::setupCustomParserNodeDelayed(const ModelNode &node, bool synchron)
{
    Q_ASSERT(isCustomParserType(node.type()));

    if (synchron) {
        setupCustomParserNode(node);
    } else {
        m_setupCustomParserList.insert(node);
        m_setupTimer.start();
    }
}

void TextToModelMerger::delayedSetup()
{
    foreach (const ModelNode node, m_setupComponentList)
        setupComponent(node);

    foreach (const ModelNode node, m_setupCustomParserList)
        setupCustomParserNode(node);
        m_setupCustomParserList.clear();
        m_setupComponentList.clear();
}

QString TextToModelMerger::textAt(const Document::Ptr &doc,
                                  const SourceLocation &location)
{
    return doc->source().mid(location.offset, location.length);
}

QString TextToModelMerger::textAt(const Document::Ptr &doc,
                                  const SourceLocation &from,
                                  const SourceLocation &to)
{
    return doc->source().mid(from.offset, to.end() - from.begin());
}
