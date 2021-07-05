#include "WebIpToLocationResolver.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QObject>
#include <QStandardPaths>
#include <QDir>
#include <QTextStream>
#include <QDataStream>
#include <QTimer>
#include <QPointF>
#include "log/Logging.h"
#include "persistence/CacheHeader.h"
#include <QSettings>
#include <QApplication>

using geo::WebIpToLocationResolver;

const QString WebIpToLocationResolver::COUNTRY_CODES_FILE = "country_codes_cache.bin";
const QString WebIpToLocationResolver::COUNTRY_NAMES_FILE_PREFIX = "country_names_cache"; // the language code will be concatenated
const QString WebIpToLocationResolver::REGION_NAMES_FILE_PREFIX = "region_names_cache"; // the language code will be concatenated
const QString WebIpToLocationResolver::LAT_LONG_CACHE_FILE = "lat_long_cache.bin";

const quint32 WebIpToLocationResolver::COUNTRY_NAMES_CACHE_REVISION = 1;
const quint32 WebIpToLocationResolver::COUNTRY_CODES_CACHE_REVISION = 1;
const quint32 WebIpToLocationResolver::REGION_NAMES_CACHE_REVISION = 1;
const quint32 WebIpToLocationResolver::LAT_LONG_CACHE_REVISION = 1;

// Alternative servers private implementation strategies
const int MaxServersAlternatives = 1;

struct IpToLocationAPI {
    QString name;
    QString url;
    QString key;
};

static IpToLocationAPI apis[MaxServersAlternatives] = {
    {"ipApi",   "http://api.ipapi.com",     "0c2691f4da94db55b32aafa61d38368a"},
};

WebIpToLocationResolver::WebIpToLocationResolver(const QDir &cacheDir) :
    currentLanguage("en"), // using english as default language
    apiServersRunning(true),
    cacheDir(cacheDir)
{
    connect(&httpClient, SIGNAL(finished(QNetworkReply*)), this, SLOT(replyFinished(QNetworkReply*)));

    loadCountryCodesFromFile();
    loadLatLongsFromFile();

    if (!needLoadTheOldCache()) {
        loadCountryNamesFromFile(currentLanguage); // loading the english country names by default
        loadRegionNamesFromFile(currentLanguage); // loading the english region names by default
    } else {
        loadOldCacheContent();
        deleteOldCacheFile();
    }
}

WebIpToLocationResolver::~WebIpToLocationResolver()
{
    saveCountryCodesToFile();
    saveCountryNamesToFile();
    saveRegionNamesToFile();
    saveLatLongsToFile();
}

void WebIpToLocationResolver::saveLatLongsToFile()
{
    if (latLongCache.isEmpty())
        return;

    QString filename = LAT_LONG_CACHE_FILE;
    quint32 cacheRevision = LAT_LONG_CACHE_REVISION;
    if (saveMapToFile(filename, latLongCache, cacheRevision))
        qCDebug(jtIpToLocation) << latLongCache.size() << " lat,lng pairs stored in " << filename;
    else
        qCritical() << "Can't save lat,lng pairs in the file " << filename;
}

void WebIpToLocationResolver::saveCountryNamesToFile()
{
    QString filename = buildFileNameFromLanguage(currentLanguage);
    quint32 cacheRevision = COUNTRY_NAMES_CACHE_REVISION;
    if (saveMapToFile(filename, countryNamesCache, cacheRevision))
        qCDebug(jtIpToLocation) << countryNamesCache.size() << " country names stored in "
                                << filename;
    else
        qCritical() << "Can't save country names in the file " << filename;
}

void WebIpToLocationResolver::saveCountryCodesToFile()
{
    quint32 cacheRevision = COUNTRY_CODES_CACHE_REVISION;
    if (saveMapToFile(COUNTRY_CODES_FILE, countryCodesCache, cacheRevision))
        qCDebug(jtIpToLocation) << countryCodesCache.size() << " country codes stored in "
                                << COUNTRY_CODES_FILE;
    else
        qCritical() << "Can't save country codes in the file " << COUNTRY_CODES_FILE;
}

void WebIpToLocationResolver::saveRegionNamesToFile()
{
    QString filename = buildRegionFileNameFromLanguage(currentLanguage);
    quint32 cacheRevision = REGION_NAMES_CACHE_REVISION;
    if (saveMapToFile(filename, regionNamesCache, cacheRevision))
        qCDebug(jtIpToLocation) << regionNamesCache.size() << " region names stored in "
                                << filename;
    else
        qCritical() << "Can't save region names in the file " << filename;
}

bool WebIpToLocationResolver::saveMapToFile(const QString &fileName, const QMap<QString, QPointF> &map, quint32 cacheHeaderRevision)
{
    if (map.isEmpty())
        return true;

    QFile cacheFile(cacheDir.absoluteFilePath(fileName));
    if (cacheFile.open(QFile::WriteOnly)) {
        QDataStream stream(&cacheFile);
        CacheHeader cacheHeader(cacheHeaderRevision);
        stream << cacheHeader;
        stream << map;
        return true;
    }
    return false;
}

bool WebIpToLocationResolver::saveMapToFile(const QString &fileName, const QMap<QString, QString> &map, quint32 cacheHeaderRevision)
{
    if (map.isEmpty())
        return true;

    QFile cacheFile(cacheDir.absoluteFilePath(fileName));
    if (cacheFile.open(QFile::WriteOnly)) {
        QDataStream stream(&cacheFile);
        CacheHeader cacheHeader(cacheHeaderRevision);
        stream << cacheHeader;
        stream << map;
        return true;
    }
    return false;
}

void WebIpToLocationResolver::replyFinished(QNetworkReply *reply)
{
    QString ip = reply->property("ip").toString();
    QString language = reply->property("language").toString();
    int retryCount = reply->property("retryCount").toInt();

    if (language != currentLanguage)
        return; // discard the received data if the language was changed since the last request.

    if (reply->error() == QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (doc.isEmpty() || !doc.isObject()) {
            qCCritical(jtIpToLocation) << "Json document is empty or is not an json object!";
            return;
        }

        QJsonObject root = doc.object();
        if (!root.contains("country_name")) {
            if (root.contains("error")) {
                auto error = root["error"].toObject();
                auto errorCode = error.contains("code") ? error["code"].toInt() : -1;
                auto errorInfo = error.contains("info") ? error["info"].toString() : "Can`t get error info!";
                if (retryCount < MaxServersAlternatives) {
                    qCDebug(jtIpToLocation) << "Error " << errorCode << " received (" << errorInfo << "), trying alternative server ...";
                    pendingRequests.clear(); // clear pending requests before try another API
                    requestDataFromWebService(ip, retryCount + 1);
                } else {
                    qCCritical(jtIpToLocation) << "All servers failed, no more alternatives available";
                    apiServersRunning = false;
                }
                return;
            } else {
                qCCritical(jtIpToLocation) << "The root json object not contains 'country_name'";
                return;
            }
        }

        if (!root.contains("country_code")) {
            qCritical() << "The root json object not contains 'country_name'";
            return;
        }

        QString countryName = root["country_name"].toString();
        QString countryCode = root["country_code"].toString();
        QString regionName = root["region_name"].toString();
        countryNamesCache.insert(countryCode, countryName);
        countryCodesCache.insert(ip, countryCode);
        regionNamesCache.insert(ip, regionName);

        if (root.contains("latitude") && root.contains("longitude")) {
            auto latitude = root["latitude"].toDouble();
            auto longitude = root["longitude"].toDouble();
            latLongCache.insert(ip, QPointF(latitude, longitude));
            qCDebug(jtIpToLocation) << "Data received IP:" << ip << " Lang:" << language << " country code:" << countryCode
                                    << " country name:" << countryName << " region:" << regionName << "lat:" << latitude << " long:" << longitude;
        } else {
            qCCritical(jtIpToLocation) << "The json 'location' object not contains 'latidude' or 'longitude' entries";
        }

        pendingRequests.remove(ip);

        emit ipResolved(ip);
    } else {
        qCDebug(jtIpToLocation) << "error requesting " << ip << ". Returning an empty location!";
    }

    reply->deleteLater();
}

// At moment the current api is http://api.ipapi.com/ (the replacement for Nekudo api). Another option is https://freegeoip.net/json/
void WebIpToLocationResolver::requestDataFromWebService(const QString &ip, uint retryCount)
{

    if (retryCount >= MaxServersAlternatives)
        return;

    pendingRequests.insert(ip);

    QNetworkRequest request;

    auto api = apis[retryCount];

    QString serviceUrl = api.url;
    QString accessKey(api.key);

    QString lang = sanitizeLanguageCode(currentLanguage);
    if (!canTranslateCountryName(lang))
        lang = "en";

    // URL FORMAT: i.e. http://api.ipapi.com/{ip}?access_key={key}&language={lang}
    request.setUrl(QUrl(QString("%1/%2?access_key=%3&language=%4").arg(serviceUrl, ip, accessKey, lang)));

    qCDebug(jtIpToLocation) << "requesting ip " << ip << " from " << request.url();

    QNetworkReply *reply = httpClient.get(request);
    reply->setProperty("ip", QVariant(ip));
    reply->setProperty("language", QVariant(currentLanguage));
    reply->setProperty("retryCount", retryCount);

    QObject::connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this,
                     SLOT(replyError(QNetworkReply::NetworkError)));
}

bool WebIpToLocationResolver::canTranslateCountryName(const QString &currentLanguage)
{
    // nekudo.com api is supporting only some languages
    static QStringList supportedCountryTranslationLanguages("en");
    supportedCountryTranslationLanguages << "de" << "es" << "fr" << "ja" << "pt" << "ru" << "zh";

    return supportedCountryTranslationLanguages.contains(sanitizeLanguageCode(currentLanguage));
}

void WebIpToLocationResolver::replyError(QNetworkReply::NetworkError e)
{
    qCritical() << "Reply error! " << e;
}

QString WebIpToLocationResolver::sanitizeLanguageCode(const QString &languageCode)
{
    if (languageCode.isEmpty())
        return "en";

    if (languageCode.size() > 2)
        return languageCode.toLower().left(2);

    return languageCode;
}

geo::Location WebIpToLocationResolver::resolve(const QString &ip, const QString &languageCode)
{
    // check for language changes
    QString code = sanitizeLanguageCode(languageCode);
    if (code != currentLanguage) {
        saveCountryNamesToFile(); // save cached country names before change the language
        saveRegionNamesToFile(); // save cached region names before change the language
        currentLanguage = code;
        loadCountryNamesFromFile(currentLanguage); // update the country names QMap
        loadRegionNamesFromFile(currentLanguage); // update the region names QMap

        pendingRequests.clear();
    }

    if (countryCodesCache.contains(ip)) {
        QString countryCode = countryCodesCache[ip];
        QString regionName = regionNamesCache[ip];
        if (countryNamesCache.contains(countryCode)) {
            QString countryName = countryNamesCache[countryCode];
            if (latLongCache.contains(ip)) {
                qreal latitude = latLongCache[ip].x();
                qreal longitude = latLongCache[ip].y();
                return Location(countryName, countryCode, regionName, latitude, longitude);
            }
        }
    }

    if (!ip.isEmpty() && !pendingRequests.contains(ip) && apiServersRunning) // avoid request pending IPs
        requestDataFromWebService(ip, 0);

    return Location();// empty location, the next request for same ip probabily return from cache
}

QString WebIpToLocationResolver::buildFileNameFromLanguage(const QString &languageCode)
{
    return COUNTRY_NAMES_FILE_PREFIX + "_" + languageCode + ".bin";
}

QString WebIpToLocationResolver::buildRegionFileNameFromLanguage(const QString &languageCode)
{
    return REGION_NAMES_FILE_PREFIX + "_" + languageCode + ".bin";
}

void WebIpToLocationResolver::loadLatLongsFromFile()
{
    QString fileName = LAT_LONG_CACHE_FILE;
    quint32 expectedCacheHeaderRevision = LAT_LONG_CACHE_REVISION;
    if (populateQMapFromFile(fileName, latLongCache, expectedCacheHeaderRevision))
        qCDebug(jtIpToLocation) << latLongCache.size() << " lat,long pairs loaded in cache";
    else
        qCritical() << "Can't open the file " << fileName;
}

void WebIpToLocationResolver::loadCountryNamesFromFile(const QString &languageCode)
{
    QString fileName = buildFileNameFromLanguage(languageCode);
    quint32 expectedCacheHeaderRevision = COUNTRY_NAMES_CACHE_REVISION;
    if (populateQMapFromFile(fileName, countryNamesCache, expectedCacheHeaderRevision))
        qCDebug(jtIpToLocation) << countryNamesCache.size() << " cached country names loaded, translated to " << languageCode;
    else
        qCritical() << "Can't open the file " << fileName;
}

void WebIpToLocationResolver::loadCountryCodesFromFile()
{
    quint32 expectedCacheHeaderRevision = COUNTRY_CODES_CACHE_REVISION;
    if (populateQMapFromFile(COUNTRY_CODES_FILE, countryCodesCache, expectedCacheHeaderRevision))
        qCDebug(jtIpToLocation) << countryCodesCache.size()
                                << " cached country codes loaded from file!";
    else
        qCritical() << "Can't open the file " << COUNTRY_CODES_FILE;
}

void WebIpToLocationResolver::loadRegionNamesFromFile(const QString &languageCode)
{
    QString fileName = buildRegionFileNameFromLanguage(languageCode);
    quint32 expectedCacheHeaderRevision = REGION_NAMES_CACHE_REVISION;
    if (populateQMapFromFile(fileName, regionNamesCache, expectedCacheHeaderRevision))
        qCDebug(jtIpToLocation) << regionNamesCache.size() << " cached region names loaded, translated to " << languageCode;
    else
        qCritical() << "Can't open the file " << fileName;
}

bool WebIpToLocationResolver::populateQMapFromFile(const QString &fileName, QMap<QString, QString> &map, quint32 expectedCacheHeaderRevision)
{
    map.clear();
    QFile cacheFile(cacheDir.absoluteFilePath(fileName));
    if (cacheFile.open(QFile::ReadOnly)) {
        QDataStream stream(&cacheFile);
        CacheHeader cacheHeader;
        stream >> cacheHeader;
        if (cacheHeader.isValid(expectedCacheHeaderRevision)) {
            stream >> map;
            return true;
        } else {
            qCritical() << "Cache header is not valid in " << fileName;
        }
    }
    return false;
}

bool WebIpToLocationResolver::populateQMapFromFile(const QString &fileName, QMap<QString, QPointF> &map, quint32 expectedCacheHeaderRevision)
{
    map.clear();
    QFile cacheFile(cacheDir.absoluteFilePath(fileName));
    if (cacheFile.open(QFile::ReadOnly)) {
        QDataStream stream(&cacheFile);
        CacheHeader cacheHeader;
        stream >> cacheHeader;
        if (cacheHeader.isValid(expectedCacheHeaderRevision)) {
            stream >> map;
            return true;
        } else {
            qCritical() << "Cache header is not valid in " << fileName;
        }
    }
    return false;
}

void WebIpToLocationResolver::loadOldCacheContent()
{
    // load cache content from the old 'cache.bin' file. This code will be delete in future versions
    QDir cacheDir(QStandardPaths::writableLocation(QStandardPaths::DataLocation));
    QFile cacheFile(cacheDir.absoluteFilePath("cache.bin"));
    if (cacheFile.open(QFile::ReadOnly)) {
        QTextStream inputStream(&cacheFile);
        while (!inputStream.atEnd())
        {
            QString line = inputStream.readLine();
            if (!line.isNull() && !line.isEmpty()) {
                QStringList parts = line.split(";");
                if (!parts.isEmpty()) {
                    QString ip = parts.at(0);
                    QString countryName = (parts.size() >= 1) ? parts.at(1) : "";
                    QString regionName = (parts.size() >= 2) ? parts.at(2) : "";
                    QString countryCode = (parts.size() >= 2) ? parts.at(2) : "";
                    countryCodesCache.insert(ip, countryCode);
                    countryNamesCache.insert(countryCode, countryName);
                    regionNamesCache.insert(ip, regionName);
                }
            }
        }
    }
}

void WebIpToLocationResolver::deleteOldCacheFile()
{
    QDir cacheDir(QStandardPaths::writableLocation(QStandardPaths::DataLocation));
    QFile cacheFile(cacheDir.absoluteFilePath("cache.bin"));
    cacheFile.remove();
}

bool WebIpToLocationResolver::needLoadTheOldCache()
{
    QDir cacheDir(QStandardPaths::writableLocation(QStandardPaths::DataLocation));
    return QFile(cacheDir.absoluteFilePath("cache.bin")).exists();
}
