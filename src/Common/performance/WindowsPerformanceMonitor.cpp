#include "PerformanceMonitor.h"
#include "../log/logging.h"

#include "Windows.h"
#include "psapi.h"
#include "fstream"
using namespace std;

//http://stackoverflow.com/questions/23143693/retrieving-cpu-load-percent-total-in-windows-with-c
//creates a static variable to convert Bytes to Megabytes
#define MB 1048576

//functions to calculate and retrieve CPU Load information
static float CalculateCPULoad();
static unsigned long long FileTimeToInt64();
float GetCPULoad();



PerformanceMonitor::PerformanceMonitor(){

    //loads the SYSTEMTIME
    SYSTEMTIME sysTime;
    //Retrieves data so that we have a way to Get it to output when using the pointers
    GetSystemTime(&sysTime);
}

//http://stackoverflow.com/questions/23143693/retrieving-cpu-load-percent-total-in-windows-with-c
static float CalculateCPULoad(unsigned long long idleTicks, unsigned long long totalTicks)
{
    static unsigned long long _previousTotalTicks = 0;
    static unsigned long long _previousIdleTicks = 0;

    unsigned long long totalTicksSinceLastTime = totalTicks - _previousTotalTicks;
    unsigned long long idleTicksSinceLastTime = idleTicks - _previousIdleTicks;


    float ret = 1.0f - ((totalTicksSinceLastTime > 0) ? ((float)idleTicksSinceLastTime) / totalTicksSinceLastTime : 0);

    _previousTotalTicks = totalTicks;
    _previousIdleTicks = idleTicks;
    return ret;
}

static unsigned long long FileTimeToInt64(const FILETIME & ft)
{
    return (((unsigned long long)(ft.dwHighDateTime)) << 32) | ((unsigned long long)ft.dwLowDateTime);
}

// Returns 1.0f for "CPU fully pinned", 0.0f for "CPU idle", or somewhere in between
// You'll need to call this at regular intervals, since it measures the load between
// the previous call and the current one.  Returns -1.0 on error.
float GetCPULoad()
{
    FILETIME idleTime, kernelTime, userTime;
    return GetSystemTimes(&idleTime, &kernelTime, &userTime) ? CalculateCPULoad(FileTimeToInt64(idleTime), FileTimeToInt64(kernelTime) + FileTimeToInt64(userTime)) : -1.0f;
}

//http://stackoverflow.com/questions/9783514/error-cannot-convert-float-to-float
double PerformanceMonitor::getCpuUsage(){

    float percent;
    percent = GetCPULoad();
            //qInfo() << percent * 100;
            return int(percent * 100);

}

PerformanceMonitor::~PerformanceMonitor()
{

}

int PerformanceMonitor::getMemmoryUsed()
{

    //https://stackoverflow.com/questions/63166/how-to-determine-cpu-and-memory-consumption-from-inside-a-process
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    bool result = GlobalMemoryStatusEx(&memInfo);
    if(result){
        return 100 - (memInfo.ullAvailPhys * 100 / memInfo.ullTotalPhys);
    }
    else{
        qWarning() << "Can't get total memory available! GlobalMemoryStatusEx fail!";
    }
    return 0;
}

int PerformanceMonitor::getBatteryUsed()
{

    //http://www.cplusplus.com/forum/beginner/72594/
    //https://stackoverflow.com/questions/22516647/c-get-battery-life-under-windows-7

        SYSTEM_POWER_STATUS status; // note not LPSYSTEM_POWER_STATUS
    GetSystemPowerStatus(&status);

    int life = status.BatteryLifePercent; //if value is 255 that means unknown

    if (life != 255) life = -life; // default as negative meaning discharging

    if (status.ACLineStatus == 1) // charging
        life = abs(life); //positive

    //qDebug() << life;

    return life;
}
