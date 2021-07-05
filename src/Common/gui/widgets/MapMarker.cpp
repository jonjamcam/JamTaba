#include "MapMarker.h"

MapMarker::MapMarker(const QString &playerName, const QString &countryName, const QString &regionName, const QPointF &latLong, const QImage &flag) :
    playerName(playerName),
    countryName(countryName),
    regionName(regionName),
    latLong(latLong),
    flag(flag)
{

}

QString MapMarker::getPlayerName() const
{
    return playerName;
}

QString MapMarker::getText() const
{
    return regionName + ", " + countryName;
}
