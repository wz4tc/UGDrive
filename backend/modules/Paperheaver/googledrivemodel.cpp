#include "googledrivemodel.h"
#include <cassert>
#include <QFile>
#include <QFileInfo>
#include <QHttpPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMimeDatabase>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include "filetreeitem.h"

namespace PageHeaver {

GoogleDriveModel::GoogleDriveModel(QObject *parent) :
    QAbstractItemModel(parent)
{
    m_fileTreeMap.insert("root", new FileTreeItem(this, "root"));
    connect(&conManager, SIGNAL(finished(QNetworkReply*)), this, SLOT(networkReplyFinished(QNetworkReply*)));
    connect(this, SIGNAL(accessTokenChanged()), this, SLOT(handleAccessTokenChanged()));
}

GoogleDriveModel::~GoogleDriveModel()
{

}

QString GoogleDriveModel::getAuthenticationRquestURL(QString scope)
{
    return QString("https://accounts.google.com/o/oauth2/auth?response_type=")
                + "code"
            + "&client_id="
                + m_clientId
            + "&redirect_uri="
                + m_redirectUri
            + "&scope="
            + scope;
}

QModelIndex GoogleDriveModel::index(int row, int column, const QModelIndex &parent) const
{
    Q_UNUSED(column);
    if (parent.isValid()) {
        FileTreeItem * item = static_cast<FileTreeItem *>(parent.internalPointer());
        return createIndex(row, column, item->getChild(row));
    } else {
        return createIndex(row, column, m_fileTreeMap["root"]);
    }
}

QModelIndex GoogleDriveModel::parent(const QModelIndex &child) const
{
    FileTreeItem * item = static_cast<FileTreeItem *>(child.internalPointer());
    if (!item) {
        return QModelIndex();
    }
    item = item->getContainingFolder();
    FileTreeItem *pparentItem = item->getContainingFolder();
    if (!pparentItem) {
        return createIndex(0, 0, item);
    } else {
        return createIndex(pparentItem->indexOf(item), 0, item);
    }
}

int GoogleDriveModel::rowCount(const QModelIndex &parent) const
{
    FileTreeItem * item = static_cast<FileTreeItem *>(parent.internalPointer());
    assert(item);
    return item->childCount();
}

int GoogleDriveModel::columnCount(const QModelIndex &parent) const
{
    return 1;
}

QVariant GoogleDriveModel::data(const QModelIndex &index, int role) const
{
    FileTreeItem * item = static_cast<FileTreeItem *>(index.internalPointer());
    switch (role) {
    case RoleFileTitle:
        return item->title();
    case RoleFileMimeType:
        return item->mimeType();
    case RoleFileAlternateLink:
        return item->alternateLink();
    }
    return QVariant();
}

QHash<int, QByteArray> GoogleDriveModel::roleNames() const
{
    static QHash<int, QByteArray> roleNames;
    if (roleNames.isEmpty()) {
        roleNames[RoleFileTitle] = "title";
        roleNames[RoleFileMimeType] = "mimeType";
        roleNames[RoleFileAlternateLink] = "alternateLink";
    }
    return roleNames;
}

QModelIndex GoogleDriveModel::index(FileTreeItem *item)
{
    qDebug() << "GoogleDriveModel::index" << item->getId();
    FileTreeItem *parent = item->getContainingFolder();
    if (parent) {
        return createIndex(parent->indexOf(item), 0, item);
    } else {
        return createIndex(0, 0, item);
    }
}

void GoogleDriveModel::requestAccessToken(QString code)
{
    QUrl url;
    url.setScheme("https");
    url.setHost("www.googleapis.com");
    url.setPath("/oauth2/v3/token");
    QUrlQuery query;
    query.addQueryItem("code", code);
    query.addQueryItem("client_id", m_clientId);
    query.addQueryItem("client_secret", m_clientSecret);
    query.addQueryItem("redirect_uri", m_redirectUri);
    query.addQueryItem("grant_type", "authorization_code");
    url.setQuery(query);
    QString dataString = url.toString(QUrl::RemoveScheme|QUrl::RemoveAuthority|
                                      QUrl::RemovePath).mid(1);
    QByteArray postData;
    postData.append(dataString.toUtf8());
    QUrl xQuery(url.toString(QUrl::RemoveQuery));

    QNetworkRequest rq(xQuery);
    conManager.post(rq, postData);
}

void GoogleDriveModel::setAuthorizationCode(QString code)
{
    qDebug() << "GoogleDriveModel::setAuthorizationCode";
    // Get access token
    requestAccessToken(code);
}

void GoogleDriveModel::setClientId(QString clientId)
{
    m_clientId = clientId;
}

void GoogleDriveModel::setClientSecret(QString clientSecret)
{
    m_clientSecret = clientSecret;
}

void GoogleDriveModel::setRedirectUri(QString redirectUri)
{
    m_redirectUri = redirectUri;
}

void GoogleDriveModel::setAccessToken(QString accessToken)
{
    if (m_accessToken != accessToken) {
        m_accessToken = accessToken;
        emit accessTokenChanged();
    }
}

void GoogleDriveModel::setRefreshToken(QString refreshToken)
{
    m_refreshToken = refreshToken;
    emit refreshTokenChanged();
}

QString GoogleDriveModel::accessToken()
{
    return m_accessToken;
}

QString GoogleDriveModel::refreshToken()
{
    return m_refreshToken;
}

void GoogleDriveModel::networkReplyFinished(QNetworkReply * reply)
{
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute) == 401) {
        //invalid credential
        emit credentialExpired();
        return;
    }
    qDebug() << "GoogleDriveModel::networkReplyFinished"
                << reply->url()
                   << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    QString response(reply->readAll());
    qDebug() << response;
    QJsonDocument doc = QJsonDocument::fromJson(response.toLatin1());
    assert(doc.isObject());
    QJsonObject object = doc.object();
    if (reply->url().toString().contains("/oauth2/v3/token")) {
        //Get token
        setAccessToken(object["access_token"].toString());
        setRefreshToken(object["refresh_token"].toString());
        emit getNewAccessToken();
    } else {
        handleJsonObject(object);
    }
}

void GoogleDriveModel::handleAccessTokenChanged()
{
    assert(!m_accessToken.isEmpty());
    //have a valid access Token now, list all files
    requestListChildren("root");
}

void GoogleDriveModel::requestListChildren(QString parentName)
{
    QUrl url;
    url.setScheme("https");
    url.setHost("www.googleapis.com");
    url.setPath("/drive/v2/files");
    QUrlQuery query;
    query.addQueryItem("key", m_clientId);
    query.addQueryItem("q", "\"root\" in parents");
    url.setQuery(query);
    QNetworkRequest rq(url);
    rq.setRawHeader(QByteArray("Authorization"), ("Bearer " + m_accessToken).toLocal8Bit());
    conManager.get(rq);
}

void GoogleDriveModel::handleJsonObject(QJsonObject object)
{
    if (object["kind"].toString() == "drive#fileList") {
        QJsonValue itemsValue = object["items"];
        assert(itemsValue.isArray());
        QJsonArray itemsArray = itemsValue.toArray();

        for (int i = 0; i < itemsArray.size(); i++) {
            handleJsonObject(itemsArray[i].toObject());
        }
    } else if (object["kind"].toString() == "drive#file") {
        QString fileId = object["id"].toString();
        FileTreeItem *parent = 0;
        if (object["parents"].toArray()[0].toObject()["isRoot"].toBool()) {
            parent = m_fileTreeMap["root"];
        }
        FileTreeItem *fileTreeItem = new FileTreeItem(this, fileId, parent->getContainingFolder());
        fileTreeItem->setTitle(object["title"].toString());
        fileTreeItem->setMimeType(object["mimeType"].toString());
        fileTreeItem->setAlternateLink(object["alternateLink"].toString());

        beginInsertRows(index(parent), parent->childCount(), parent->childCount());
        m_fileTreeMap[fileId] = fileTreeItem;
        parent->addChild(fileTreeItem);
        endInsertRows();
    }
}

void GoogleDriveModel::requestRefreshToken()
{
    qDebug() << "TODO GoogleDriveModel::requestRefreshToken";
}

void GoogleDriveModel::requestUploadFile(QString localFileUrl)
{
    QUrl url;
    url.setScheme("https");
    url.setHost("www.googleapis.com");
    url.setPath(QString("/upload/drive/v2/files"));
    QUrlQuery query;
    query.addQueryItem("uploadType", "multipart");
    query.addQueryItem("key", m_clientId);
    url.setQuery(query);
    QNetworkRequest rq(url);

    QHttpMultiPart *multiPart = new QHttpMultiPart();

    QFile f(localFileUrl.mid(6));
    QFileInfo fileInfo(f);
    QHttpPart textPart;
    textPart.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QJsonObject object;
    object.insert("title", QJsonValue(fileInfo.baseName()));
    textPart.setBody(QJsonDocument(object).toJson());

    QHttpPart contentPart;
    contentPart.setHeader(QNetworkRequest::ContentTypeHeader, QMimeDatabase().mimeTypeForFile(f.fileName()).name());
    f.open(QFile::ReadOnly);
    contentPart.setBody(f.readAll());
    f.close();

    multiPart->append(textPart);
    multiPart->append(contentPart);

    rq.setRawHeader(QByteArray("Authorization"), ("Bearer " + m_accessToken).toLocal8Bit());
    QNetworkReply * reply = conManager.post(rq, multiPart);
    multiPart->setParent(reply);
}

}
