#include "IpToLocationResolver.h"

#include "log/Logging.h"
#include <QRegularExpression>

using geo::Location;
using geo::IpToLocationResolver;
using geo::NullIpToLocationResolver;

Location::Location() :
    countryName("UNKNOWN"),
    countryCode("UNKNOWN"),
    regionName("UNKNOWN"),
    latitude(-200),
    longitude(-200)
{
}

bool Location::isUnknown() const
{
    return countryName == "UNKNOWN" && countryCode == "UNKNOWN" && regionName == "UNKNOWN";
}

Location::Location(const QString &country, const QString &countryCode,const QString &region, double latitude, double longitude) :
    countryName(sanitize(country)),
    countryCode(sanitize(countryCode)),
    regionName(sanitize(region)),
    latitude(latitude),
    longitude(longitude)
{
    //
}

QString Location::sanitize(const QString &inputString)
{
    static QString htmlTagsPattern("<.+>");
    static QRegularExpression regex(htmlTagsPattern);
    return QString(inputString).replace(regex, QStringLiteral(""));
}

IpToLocationResolver::~IpToLocationResolver()
{
    qCDebug(jtIpToLocation) << "IpToLocationResolver destructor";
}

Location NullIpToLocationResolver::resolve(const QString &ip, const QString &languageCode)
{
    Q_UNUSED(ip)
    Q_UNUSED(languageCode)
    return geo::Location();
}
