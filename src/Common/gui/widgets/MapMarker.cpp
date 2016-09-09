#include "MapMarker.h"

MapMarker::MapMarker(const QString &playerName, const QString &countryName, const QPointF &latLong) :
    name(playerName),
    countryName(countryName),
    latLong(latLong)//,
    //flag(flag)
{

}

QString MapMarker::getText() const
{
    return name;// + " (" + countryName + ")";
}
